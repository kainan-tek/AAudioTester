package com.example.aaudiotester.recorder

import android.content.Context
import android.util.Log
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioConstants
import com.example.aaudiotester.common.AAudioEngine
import java.util.concurrent.Executor

/** Recording engine: AAudio input + WAV writing. */
class AAudioRecorder(context: Context, private val nativeExecutor: Executor) : AAudioEngine {

    companion object {
        private const val TAG = "AAudioRecorder"
        init {
            // Fail fast with UnsatisfiedLinkError at load time (crash point == cause point); don't swallow errors and defer to external call sites
            System.loadLibrary("aaudiotester")
        }
    }

    private enum class State { IDLE, RECORDING, ERROR }

    private val appContext = context.applicationContext
    private var currentConfig: AAudioConfig = AAudioConfig()
    private var listener: AAudioEngine.Listener? = null

    @Volatile
    private var state = State.IDLE

    @Volatile
    private var errored = false  // This session already reported an error: late started callbacks are ignored (error-first, terminal)

    @Volatile
    private var nativeBound = false  // initializeNative may fail on rotation rebuild if the old session hasn't stopped; retried at start

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

    override fun setListener(listener: AAudioEngine.Listener?) {
        this.listener = listener
    }

    override fun setAudioConfig(config: AAudioConfig) {
        if (state == State.RECORDING) {
            Log.w(TAG, "Cannot change configuration while recording")
            return
        }
        // Paths not ending in .wav are treated as directories by the native layer, which auto-generates a filename; invalid paths report a [FILE] error at start
        val audioFilePath = config.audioFilePath.ifBlank { getDefaultDirectory() }
        // Native is the source of truth: a rejected config must not update currentConfig, or start
        // would use parameters that were never applied. Rejection is defensive — the UI locks the
        // spinner across the whole busy window, so no switch can be in flight; expect true here.
        val applied = setNativeConfig(
            AAudioConstants.getInputPreset(config.inputPreset),
            config.sampleRate,
            config.channelCount,
            AAudioConstants.getFormatFromBitDepth(config.bitDepth),
            AAudioConstants.getPerformanceMode(config.performanceMode),
            AAudioConstants.getSharingMode(config.sharingMode),
            audioFilePath
        )
        if (applied) {
            currentConfig = config
        } else {
            Log.w(TAG, "Native rejected configuration, keeping previous")
        }
    }

    override fun start(): Boolean {
        if (!ensureNativeBound()) {
            val error = "${AAudioConstants.ErrorTypes.STREAM} Native initialization failed"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }
        if (state == State.RECORDING) {
            Log.w(TAG, "Already recording")
            listener?.onError("Already recording")
            return false
        }
        if (state == State.ERROR) state = State.IDLE
        errored = false  // reset for a new session

        if (!AAudioConstants.isValidSampleRate(currentConfig.sampleRate)) {
            val error = "${AAudioConstants.ErrorTypes.PARAM} Invalid sample rate: ${currentConfig.sampleRate}"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }
        if (!AAudioConstants.isValidChannelCount(currentConfig.channelCount)) {
            val error = "${AAudioConstants.ErrorTypes.PARAM} Invalid channel count: ${currentConfig.channelCount}"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }
        if (!AAudioConstants.isValidFormat(currentConfig.bitDepth)) {
            val error = "${AAudioConstants.ErrorTypes.PARAM} Invalid bit depth: ${currentConfig.bitDepth}"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }

        val result = startNativeRecording()
        if (result) {
            // The native true return IS the started signal (synchronous, same executor thread —
            // no separate started callback to lose). A terminal RT error may have raced the start:
            // error-first, ignore a late start
            if (!errored) {
                state = State.RECORDING
                listener?.onStarted()
                Log.i(TAG, "Recording started successfully")
            }
        } else if (state == State.IDLE) {
            // A real native failure always delivers onError synchronously (state becomes ERROR before
            // this returns). state still IDLE here means native returned false without notifying —
            // the cross-language contract is broken; recover the UI instead of sticking it forever
            listener?.onError("${AAudioConstants.ErrorTypes.STREAM} Native start failed without error callback")
        }
        return result
    }

    override fun stop() {
        // Also entered from ERROR state: cleans up the stream/file/writer thread left by an error (triggered by Fragment.onError)
        if (state == State.IDLE) return
        Log.d(TAG, "Stopping recording")
        // True = a stopped/error notice already reached Kotlin natively (latch owned by an async
        // path, or delivered by this stop); false = delivery failed — run the same completion as
        // the native callback so the UI recovers (return-value protocol, same as start)
        if (!stopNativeRecording()) {
            onNativeRecordingStopped()
        }
        state = State.IDLE
    }

    override fun isActive(): Boolean = state == State.RECORDING

    override fun release() {
        if (state == State.RECORDING) stop()
        listener = null
        try {
            releaseNative()
        } catch (e: Exception) {
            Log.e(TAG, "Error releasing native resources", e)
        }
        // The instance is dead: settle a leftover ERROR state to IDLE so a late queued stop()
        // (native-thread error racing recreation) hits the IDLE gate instead of touching native
        // state that may already belong to a newer instance
        state = State.IDLE
        Log.d(TAG, "AAudioRecorder resources released")
    }

    private fun getDefaultDirectory(): String {
        return appContext.getExternalFilesDir(null)?.absolutePath
            ?: appContext.filesDir.absolutePath
    }

    // Native methods
    private external fun initializeNative(): Boolean
    private external fun setNativeConfig(
        inputPreset: Int, sampleRate: Int, channelCount: Int, format: Int,
        performanceMode: Int, sharingMode: Int, audioFilePath: String
    ): Boolean
    private external fun startNativeRecording(): Boolean
    private external fun stopNativeRecording(): Boolean
    private external fun releaseNative()

    // Native layer callbacks (JNI-invoked; onStopped is also self-run by stop() on delivery failure)
    private fun onNativeRecordingStopped() {
        state = State.IDLE
        listener?.onStopped()
        Log.i(TAG, "Recording stopped")
    }

    @Suppress("unused")
    private fun onNativeRecordingError(error: String) {
        errored = true  // terminal error state for this session
        state = State.ERROR
        listener?.onError(error)
        Log.e(TAG, "Recording error: $error")
    }
}
