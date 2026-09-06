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
 *   any  --> TERMINAL       (onNativeError / onNativeStopped — see those for their differing semantics)
 *   any  --> IDLE           (stop / release settle)
 *
 * Subclasses keep only what genuinely differs: their JNI entry points (bound by class name, so the
 * external declarations and the JNI-named callback forwarders must stay in the subclass), config
 * handling, and start preconditions via [beforeStartNative].
 */
abstract class AAudioEngineBase(protected val nativeExecutor: Executor) : AAudioEngine {

    companion object {
        // Construction-time log tag: the init task below can run on the executor while the subclass
        // properties (logTag included) are still initializing, so the failure log must not read them
        private const val TAG = "AAudioEngineBase"
    }

    private enum class SessionState { IDLE, ACTIVE, TERMINAL }

    private val state = AtomicReference(SessionState.IDLE)

    @Volatile
    private var listener: AAudioEngine.Listener? = null  // written by setListener (main thread) and release (executor), dereferenced from native worker threads (onNativeError/onNativeStopped) — JMM-shared

    // The config the native layer actually holds (updated only on an applied setAudioConfig —
    // see onConfigApplied); read by the subclasses' beforeStartNative
    protected var currentConfig: AAudioConfig = AAudioConfig()
        private set

    /** The single writer of [currentConfig]: a rejected config must not update it. */
    protected fun onConfigApplied(config: AAudioConfig, applied: Boolean) {
        // Native is the source of truth: a rejected config must not update currentConfig, or start
        // would use parameters that were never applied. Rejection is defensive — the UI locks the
        // spinner across the whole busy window, so no switch can be in flight; expect true here.
        if (applied) {
            currentConfig = config
        } else {
            Log.w(logTag, "Native rejected configuration, keeping previous")
        }
    }

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
        executeGuarded("native bind") {
            nativeBound = initializeNative()
        }
    }

    /**
     * Submits to the native executor behind the JNI-boundary Throwable guard: a pending exception
     * at the boundary (GetMethodID failure, NewGlobalRef OOM) surfaces as a Kotlin throwable and
     * would kill the process if submitted bare. Log-only on failure — callers with UI to recover
     * surface it via the listener callbacks (the Fragment's onEngineThread has its own guard with
     * a different, UI-recovering policy).
     */
    protected fun executeGuarded(what: String, task: () -> Unit) {
        nativeExecutor.execute {
            try {
                task()
            } catch (t: Throwable) {
                Log.e(TAG, "Native task failed: $what", t)
            }
        }
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
        // Re-arm from a previous session's terminal state. Safe: the terminal notice releases the
        // session's resources (onSessionTerminated) BEFORE publishing TERMINAL, so this CAS — which
        // must observe that volatile write — also orders after the release; nothing of the old
        // session can land after this settle. (Native start/stop additionally join the notice
        // thread before spawning a new one.)
        state.compareAndSet(SessionState.TERMINAL, SessionState.IDLE)

        beforeStartNative()?.let { error ->
            Log.e(logTag, error)
            listener?.onError(error)
            return false
        }

        val result = try {
            startNative()
        } catch (t: Throwable) {
            // The postlude below only runs on a returned result: a JNI-boundary throwable here
            // would skip it and leak the resources acquired in beforeStartNative (the player's
            // audio focus) — release them and re-raise
            onSessionTerminated()
            throw t
        }
        return if (result) {
            // The native true return IS the started signal (synchronous, same executor thread —
            // no separate started callback to lose). A terminal notice delivered from a native
            // thread while startNative ran has already moved the state off IDLE: the CAS loses,
            // the late start stays silent, and the terminal notice owns the UI. This atomic
            // transition is the whole race fix — never replace it with a read-then-write.
            val casWon = state.compareAndSet(SessionState.IDLE, SessionState.ACTIVE)
            if (casWon) {
                listener?.onStarted()
                Log.i(logTag, "${sessionLabel.replaceFirstChar { it.uppercase() }} started successfully")
            }
            // Truthful started signal: a CAS lost to a racing terminal notice means the session
            // is already dead (the notice owns the UI) — the raw native result would call it started
            casWon
        } else {
            onSessionTerminated()  // release what beforeStartNative acquired (focus); idempotent
            // A real native failure always delivers onError synchronously (state becomes ERROR before
            // this returns). state still IDLE here means native returned false without notifying —
            // the cross-language contract is broken; recover the UI instead of sticking it forever
            if (state.get() == SessionState.IDLE) {
                listener?.onError("${AAudioConstants.ErrorTypes.STREAM} Native start failed without error callback")
            }
            result
        }
    }

    final override fun stop() {
        // Also entered from a terminal state: cleans up the stream/file/thread left by an error (triggered by Fragment.onError)
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
        try {
            // Unconditional: the IDLE gate makes it a no-op on a settled session, and from TERMINAL
            // it runs the same cleanup the Fragment's onError protocol would queue — so the native
            // teardown happens through the front door and releaseNative's internal stop never lands
            // on a settled state (the re-entrant onNativeStopped residual is gone).
            stop()
            listener = null
        } finally {
            // Everything here must survive the JNI-boundary throwables above (stop's stopNative is
            // OOM-able): skipping onSessionTerminated leaks the session's audio focus, skipping
            // releaseNative leaks the stream/hardware session with the IDLE gate below blocking any
            // later recovery stop. releaseNative is idempotent over partially-torn native state
            // (stale-release guard + stream/wav null checks). Errors still propagate to the outer
            // guard — but reclaimed and settled. Narrowed residual: only when stop() itself threw
            // mid-teardown can releaseNative's internal stop still re-enter onNativeStopped after
            // this settle (listener not yet nulled; the notice is truthful and UI-guarded).
            onSessionTerminated()
            state.set(SessionState.IDLE)
            try {
                releaseNative()
            } catch (e: Exception) {
                Log.e(logTag, "Error releasing native resources", e)
            }
            Log.d(logTag, "${javaClass.simpleName} resources released")
        }
    }

    // Native layer callback (JNI-invoked via the subclass forwarders, native thread): terminal for
    // this session — a start in flight must lose its CAS and stay silent. Semantics: an error
    // leaves native state to clean up, surfaced via onError (Fragment.onError queues that cleanup
    // stop when isActive() is false).
    protected fun onNativeError(error: String) {
        // Session resources are released BEFORE the terminal publish: start()'s re-arm CAS must
        // observe this TERMINAL write, so everything before it (the abandon) happens-before the
        // next session's resource acquisition — a stale native-thread abandon can never reach the
        // next session's freshly acquired resources (e.g. the player's audio focus).
        onSessionTerminated()
        state.set(SessionState.TERMINAL)
        listener?.onError(error)
        Log.e(logTag, "${sessionLabel.replaceFirstChar { it.uppercase() }} error: $error")
    }

    // Native layer callback (JNI-invoked; also self-run by stop() on delivery failure). Semantics:
    // a stopped/EOF notice means native already tore the session down, surfaced via onStopped.
    protected fun onNativeStopped() {
        // Same ordering as onNativeError: release before the TERMINAL publish (see there)
        onSessionTerminated()
        state.set(SessionState.TERMINAL)
        listener?.onStopped()
        Log.i(logTag, "${sessionLabel.replaceFirstChar { it.uppercase() }} stopped")
    }
}
