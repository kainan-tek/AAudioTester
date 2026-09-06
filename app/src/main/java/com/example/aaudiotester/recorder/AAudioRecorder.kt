package com.example.aaudiotester.recorder

import android.content.Context
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioConstants
import com.example.aaudiotester.common.AAudioEngineBase
import java.util.concurrent.Executor

/** Recording engine: AAudio input + WAV writing (session protocol shared in AAudioEngineBase). */
class AAudioRecorder(context: Context, nativeExecutor: Executor) :
    AAudioEngineBase(nativeExecutor) {

    companion object {
        private const val TAG = "AAudioRecorder"
        init {
            // Fail fast with UnsatisfiedLinkError at load time (crash point == cause point); don't swallow errors and defer to external call sites
            System.loadLibrary("aaudiotester")
        }
    }

    override val logTag = TAG
    override val sessionLabel = "recording"
    override val alreadyActiveNotice = "Already recording"

    private val appContext = context.applicationContext

    override fun applyNativeConfig(config: AAudioConfig): Boolean {
        // Paths not ending in .wav are treated as directories by the native layer, which auto-generates a filename; invalid paths report a [FILE] error at start
        val audioFilePath = config.audioFilePath.ifBlank { getDefaultDirectory() }
        return setNativeConfig(
            AAudioConstants.getInputPreset(config.inputPreset),
            config.sampleRate,
            config.channelCount,
            AAudioConstants.getFormatFromBitDepth(config.bitDepth),
            AAudioConstants.getPerformanceMode(config.performanceMode),
            AAudioConstants.getSharingMode(config.sharingMode),
            audioFilePath
        )
    }

    override fun beforeStartNative(): String? {
        if (!AAudioConstants.isValidSampleRate(currentConfig.sampleRate)) {
            return "${AAudioConstants.ErrorTypes.PARAM} Invalid sample rate: ${currentConfig.sampleRate}"
        }
        if (!AAudioConstants.isValidChannelCount(currentConfig.channelCount)) {
            return "${AAudioConstants.ErrorTypes.PARAM} Invalid channel count: ${currentConfig.channelCount}"
        }
        if (!AAudioConstants.isValidFormat(currentConfig.bitDepth)) {
            return "${AAudioConstants.ErrorTypes.PARAM} Invalid bit depth: ${currentConfig.bitDepth}"
        }
        return null
    }

    private fun getDefaultDirectory(): String {
        return appContext.getExternalFilesDir(null)?.absolutePath
            ?: appContext.filesDir.absolutePath
    }

    // Native methods (JNI binds by this class's name; the base's protocol hooks route here)
    external override fun initializeNative(): Boolean
    external override fun startNative(): Boolean
    external override fun stopNative(): Boolean
    external override fun releaseNative()

    private external fun setNativeConfig(
        inputPreset: Int, sampleRate: Int, channelCount: Int, format: Int,
        performanceMode: Int, sharingMode: Int, audioFilePath: String
    ): Boolean

    // Native layer callbacks: JNI binds by these exact names (aaudio_common.h); forward to the shared protocol
    @Suppress("unused")
    private fun onNativeRecordingStopped() = onNativeStopped()

    @Suppress("unused")
    private fun onNativeRecordingError(error: String) = onNativeError(error)
}
