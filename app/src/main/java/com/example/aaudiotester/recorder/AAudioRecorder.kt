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
            // 加载失败时立即抛 UnsatisfiedLinkError（崩溃点即原因点），不吞错推迟到 external 调用处
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
    private var errored = false  // 本会话已上报错误：迟到的 started 一律忽略（错误优先、终态）

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
        currentConfig = config
        // 非 .wav 结尾的路径由 native 层解释为目录并自动生成文件名；无效路径在 start 时报 [FILE] 错误
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
        errored = false  // 新会话复位

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
        // ERROR 态也进入：清理错误残留的流/文件/写线程（由 Fragment.onError 触发）
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

    // 原生层回调
    @Suppress("unused")
    private fun onNativeRecordingStarted() {
        if (errored) return  // 错误优先：native 已停止，忽略竞态中迟到的 started
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
        errored = true  // 会话内错误终态
        state = State.ERROR
        listener?.onError(error)
        Log.e(TAG, "Recording error: $error")
    }
}
