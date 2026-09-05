package com.example.aaudiotester.common

import android.util.Log
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicReference

/**
 * Shared session protocol for the native engines (playback / recording): the state machine both
 * engines used to hand-sync. Also owns the executor-ordered native binding.
 *
 * State machine (single authoritative copy — the CAS in [start] is what closes the race where a
 * terminal notice delivered from a native thread during startNative used to be overwritten):
 *
 *   IDLE --CAS--> ACTIVE    (start postlude; the ONLY writer of ACTIVE, one atomic transition)
 *   any  --> ERROR          (onNativeError, native thread: needs a cleanup stop via Fragment.onError)
 *   any  --> CLOSED         (onNativeStopped, native thread or stop's delivery-failure fallback)
 *   any  --> IDLE           (stop / release settle)
 *
 * ERROR vs CLOSED: an error leaves native state to clean up (Fragment.onError queues a stop when
 * isActive() is false), while a stopped/EOF notice means native already tore the session down —
 * hence two distinct terminal states, and start() settles both back to IDLE before re-arming.
 *
 * Subclasses keep only what genuinely differs: their JNI entry points (bound by class name, so the
 * external declarations and the JNI-named callback forwarders must stay in the subclass), config
 * handling, and start preconditions via [beforeStartNative].
 */
abstract class AAudioEngineBase(protected val nativeExecutor: Executor) : AAudioEngine {

    private enum class SessionState { IDLE, ACTIVE, ERROR, CLOSED }

    private val state = AtomicReference(SessionState.IDLE)
    private var listener: AAudioEngine.Listener? = null

    @Volatile
    private var nativeBound = false  // initializeNative may fail on rotation rebuild if the old session hasn't stopped; retried at start

    protected abstract val logTag: String

    /** "playback" / "recording": composes the log texts ("Stopping playback", "Playback stopped", ...). */
    protected abstract val sessionLabel: String

    /** "Already playing" / "Already recording": log text and listener notice of a start while active. */
    protected abstract val alreadyActiveNotice: String

    /**
     * Preconditions and session-resource acquisition before the native start (parameter validation,
     * audio focus). Return an error string to abort — the string is delivered via onError verbatim.
     */
    protected abstract fun beforeStartNative(): String?

    protected abstract fun initializeNative(): Boolean
    protected abstract fun startNative(): Boolean
    protected abstract fun stopNative(): Boolean
    protected abstract fun releaseNative()

    /** Release session resources on teardown (e.g. audio focus); must be idempotent. */
    protected open fun onSessionTerminated() {}

    init {
        // Bind on the shared executor, not the constructing (main) thread: every native entry stays
        // FIFO-ordered, so a rotation rebuild's initialize runs after the old instance's stop/release
        nativeExecutor.execute { nativeBound = initializeNative() }
    }

    /** Binding is retryable: on rotation rebuild an unstopped old session can fail the first bind; rebind at start (native is idempotent, repeated calls are safe) */
    private fun ensureNativeBound(): Boolean {
        if (!nativeBound) nativeBound = initializeNative()
        return nativeBound
    }

    final override fun setListener(listener: AAudioEngine.Listener?) {
        this.listener = listener
    }

    final override fun isActive(): Boolean = state.get() == SessionState.ACTIVE

    final override fun start(): Boolean {
        if (!ensureNativeBound()) {
            val error = "${AAudioConstants.ErrorTypes.STREAM} Native initialization failed"
            Log.e(logTag, error)
            listener?.onError(error)
            return false
        }
        if (state.get() == SessionState.ACTIVE) {
            Log.w(logTag, alreadyActiveNotice)
            listener?.onError(alreadyActiveNotice)
            return false
        }
        // Re-arm from a previous session's terminal state. Safe: the session's reader thread has
        // exited by then (its terminal notice is that thread's last act; native start/stop also
        // join it before spawning a new one), so no write can land after this settle.
        settleTerminalState()

        beforeStartNative()?.let { error ->
            Log.e(logTag, error)
            listener?.onError(error)
            return false
        }

        val result = startNative()
        if (result) {
            // The native true return IS the started signal (synchronous, same executor thread —
            // no separate started callback to lose). A terminal notice delivered from a native
            // thread while startNative ran has already moved the state off IDLE: the CAS loses,
            // the late start stays silent, and the terminal notice owns the UI. This atomic
            // transition is the whole race fix — never replace it with a read-then-write.
            if (state.compareAndSet(SessionState.IDLE, SessionState.ACTIVE)) {
                listener?.onStarted()
                Log.i(logTag, "${sessionLabel.replaceFirstChar { it.uppercase() }} started successfully")
            }
        } else {
            onSessionTerminated()  // release what beforeStartNative acquired (focus); idempotent
            // A real native failure always delivers onError synchronously (state becomes ERROR before
            // this returns). state still IDLE here means native returned false without notifying —
            // the cross-language contract is broken; recover the UI instead of sticking it forever
            if (state.get() == SessionState.IDLE) {
                listener?.onError("${AAudioConstants.ErrorTypes.STREAM} Native start failed without error callback")
            }
        }
        return result
    }

    final override fun stop() {
        // Also entered from ERROR state: cleans up the stream/file/thread left by an error (triggered by Fragment.onError)
        if (state.get() == SessionState.IDLE) return
        Log.d(logTag, "Stopping $sessionLabel")
        // True = a stopped/error notice already reached Kotlin natively (latch owned by an async
        // path, or delivered by this stop); false = delivery failed — run the same completion as
        // the native callback so the UI recovers (return-value protocol, same as start)
        if (!stopNative()) {
            onNativeStopped()
        }
        state.set(SessionState.IDLE)
    }

    final override fun release() {
        if (state.get() == SessionState.ACTIVE) stop()
        onSessionTerminated()
        listener = null
        try {
            releaseNative()
        } catch (e: Exception) {
            Log.e(logTag, "Error releasing native resources", e)
        }
        // The instance is dead: settle a leftover terminal state to IDLE so a late queued stop()
        // (native-thread error racing recreation) hits the IDLE gate instead of touching native
        // state that may already belong to a newer instance
        state.set(SessionState.IDLE)
        Log.d(logTag, "${javaClass.simpleName} resources released")
    }

    // Native layer callback (JNI-invoked via the subclass forwarders, native thread): terminal for
    // this session — a start in flight must lose its CAS and stay silent
    protected fun onNativeError(error: String) {
        state.set(SessionState.ERROR)
        onSessionTerminated()
        listener?.onError(error)
        Log.e(logTag, "${sessionLabel.replaceFirstChar { it.uppercase() }} error: $error")
    }

    // Native layer callback (JNI-invoked; also self-run by stop() on delivery failure)
    protected fun onNativeStopped() {
        state.set(SessionState.CLOSED)
        onSessionTerminated()
        listener?.onStopped()
        Log.i(logTag, "${sessionLabel.replaceFirstChar { it.uppercase() }} stopped")
    }

    private fun settleTerminalState() {
        state.compareAndSet(SessionState.ERROR, SessionState.IDLE)
        state.compareAndSet(SessionState.CLOSED, SessionState.IDLE)
    }
}
