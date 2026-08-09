package com.example.aaudiotester.recorder

import android.content.Context
import android.util.Log
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioConstants
import com.example.aaudiotester.common.AAudioEngine

/** 录音引擎：AAudio 输入 + WAV 写出。 */
class AAudioRecorder(context: Context) : AAudioEngine {

    companion object {
        private const val TAG = "AAudioRecorder"
        init {
            try {
                System.loadLibrary("aaudiotester")
                Log.d(TAG, "Native library loaded")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load native library", e)
            }
        }
    }

    private enum class State { IDLE, RECORDING, ERROR }

    private val appContext = context.applicationContext
    private var currentConfig: AAudioConfig = AAudioConfig()
    private var listener: AAudioEngine.Listener? = null

    @Volatile
    private var state = State.IDLE

    init {
        initializeNative()
    }

    override fun setListener(listener: AAudioEngine.Listener?) {
        this.listener = listener
    }

    override fun setAudioConfig(config: AAudioConfig) {
        if (state == State.RECORDING) {
            Log.w(TAG, "Cannot change configuration while recording")
            return
        }
        if (config.audioFilePath.isNotBlank() && !config.audioFilePath.endsWith(".wav")) {
            Log.e(TAG, "Invalid output path: must be empty or end with .wav")
            return
        }
        currentConfig = config
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
        if (state == State.RECORDING) {
            Log.w(TAG, "Already recording")
            listener?.onError("Already recording")
            return false
        }
        if (state == State.ERROR) state = State.IDLE

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
        if (state != State.RECORDING) return
        Log.d(TAG, "Stopping recording")
        stopNativeRecording()
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

    // 原生层回调
    @Suppress("unused")
    private fun onNativeRecordingStarted() {
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
        state = State.ERROR
        listener?.onError(error)
        Log.e(TAG, "Recording error: $error")
    }
}
