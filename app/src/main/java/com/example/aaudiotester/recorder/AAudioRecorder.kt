package com.example.aaudiotester.recorder

import android.content.Context
import android.util.Log
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioConstants
import com.example.aaudiotester.common.AAudioEngine

/** Recording engine: AAudio input + WAV writing. */
class AAudioRecorder(context: Context) : AAudioEngine {

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
        nativeBound = initializeNative()
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
        currentConfig = config
        // Paths not ending in .wav are treated as directories by the native layer, which auto-generates a filename; invalid paths report a [FILE] error at start
        val audioFilePath = config.audioFilePath.ifBlank { getDefaultDirectory() }
        setNativeConfig(
            AAudioConstants.getInputPreset(config.inputPreset),
            config.sampleRate,
            config.channelCount,
            AAudioConstants.getFormatFromBitDepth(config.format),
            AAudioConstants.getPerformanceMode(config.performanceMode),
            AAudioConstants.getSharingMode(config.sharingMode),
            audioFilePath
        )
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
        if (!AAudioConstants.isValidFormat(currentConfig.format)) {
            val error = "${AAudioConstants.ErrorTypes.PARAM} Invalid bit depth: ${currentConfig.format}"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }

        return startNativeRecording()
    }

    override fun stop() {
        // Also entered from ERROR state: cleans up the stream/file/writer thread left by an error (triggered by Fragment.onError)
        if (state == State.IDLE) return
        Log.d(TAG, "Stopping recording")
        stopNativeRecording()
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

    // Native layer callbacks
    @Suppress("unused")
    private fun onNativeRecordingStarted() {
        if (errored) return  // error-first: native already stopped, ignore a started arriving late in a race
        state = State.RECORDING
        listener?.onStarted()
        Log.i(TAG, "Recording started successfully")
    }

    @Suppress("unused")
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
