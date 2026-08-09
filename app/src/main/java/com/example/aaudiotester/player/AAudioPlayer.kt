package com.example.aaudiotester.player

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.util.Log
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioConstants
import com.example.aaudiotester.common.AAudioEngine
import com.example.aaudiotester.common.AssetExtractor

/** 播放引擎：AAudio 输出 + 音频焦点管理。 */
class AAudioPlayer(context: Context) : AAudioEngine {

    companion object {
        private const val TAG = "AAudioPlayer"
        init {
            try {
                System.loadLibrary("aaudiotester")
                Log.d(TAG, "Native library loaded")
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "Failed to load native library", e)
            }
        }
    }

    private enum class State { IDLE, PLAYING, ERROR }

    private val appContext = context.applicationContext
    private val audioManager = appContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var currentConfig: AAudioConfig = AAudioConfig()
    private var listener: AAudioEngine.Listener? = null
    private var audioFocusRequest: AudioFocusRequest? = null

    @Volatile
    private var state = State.IDLE

    private val audioFocusChangeListener = AudioManager.OnAudioFocusChangeListener { focusChange ->
        when (focusChange) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                Log.d(TAG, "Audio focus lost, stopping playback")
                stop()
            }
        }
    }

    init {
        initializeNative(resolveCurrentPath())
    }

    private fun resolveCurrentPath(): String {
        val path = currentConfig.audioFilePath.ifBlank { AAudioConstants.DEFAULT_ASSET }
        return try {
            AssetExtractor.resolveAssetPath(appContext, path)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to resolve asset path: $path", e)
            path  // 回退原始路径，让 native 报 [FILE] 错误走正常错误路径
        }
    }

    override fun setListener(listener: AAudioEngine.Listener?) {
        this.listener = listener
    }

    override fun setAudioConfig(config: AAudioConfig) {
        if (state == State.PLAYING) {
            Log.w(TAG, "Cannot change configuration while playing")
            return
        }
        currentConfig = config
        setNativeConfig(
            AAudioConstants.getUsage(config.usage),
            AAudioConstants.getContentType(config.contentType),
            AAudioConstants.getPerformanceMode(config.performanceMode),
            AAudioConstants.getSharingMode(config.sharingMode),
            resolveCurrentPath()
        )
    }

    override fun start(): Boolean {
        if (state == State.PLAYING) {
            Log.w(TAG, "Already playing")
            listener?.onError("Already playing")
            return false
        }
        if (state == State.ERROR) state = State.IDLE

        val audioPath = resolveCurrentPath()
        if (audioPath.isBlank() || !audioPath.lowercase().endsWith(".wav")) {
            val error = "${AAudioConstants.ErrorTypes.PARAM} Invalid audio file path: must end with .wav"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }

        if (!requestAudioFocus()) {
            Log.e(TAG, "Unable to obtain audio focus")
            listener?.onError("${AAudioConstants.ErrorTypes.FOCUS} Unable to obtain audio focus")
            return false
        }

        val result = startNativePlayback()
        if (!result) abandonAudioFocus()
        return result
    }

    override fun stop() {
        if (state != State.PLAYING) return
        Log.d(TAG, "Stopping playback")
        stopNativePlayback()
        abandonAudioFocus()
    }

    override fun isActive(): Boolean = state == State.PLAYING

    override fun release() {
        if (state == State.PLAYING) stop()
        listener = null
        try {
            releaseNative()
        } catch (e: Exception) {
            Log.e(TAG, "Error releasing native resources", e)
        }
        Log.d(TAG, "AAudioPlayer resources released")
    }

    private fun requestAudioFocus(): Boolean {
        // AAOS 系统 usage（>=1000）AudioAttributes 无法表示（@hide，setUsage 会抛 IllegalArgumentException），
        // 且系统 usage 属车辆关键音频、不依赖普通焦点管理，故跳过焦点请求。
        if (AAudioConstants.getUsage(currentConfig.usage) >= 1000) return true

        val audioAttributes = AudioAttributes.Builder()
            .setUsage(AAudioConstants.getUsage(currentConfig.usage))
            .setContentType(AAudioConstants.getContentType(currentConfig.contentType))
            .build()

        val focusType = determineFocusType()
        audioFocusRequest = AudioFocusRequest.Builder(focusType)
            .setAudioAttributes(audioAttributes)
            .setAcceptsDelayedFocusGain(false)
            .setOnAudioFocusChangeListener(audioFocusChangeListener)
            .build()

        val result = audioManager.requestAudioFocus(audioFocusRequest!!)
        return result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    private fun determineFocusType(): Int = when {
        currentConfig.usage.contains("EMERGENCY") || currentConfig.usage.contains("SAFETY") ->
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
        currentConfig.usage.contains("NAVIGATION") || currentConfig.usage.contains("ANNOUNCEMENT") ->
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK
        currentConfig.usage.contains("VOICE_COMMUNICATION") ->
            AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
        else -> AudioManager.AUDIOFOCUS_GAIN
    }

    private fun abandonAudioFocus() {
        audioFocusRequest?.let { request ->
            audioManager.abandonAudioFocusRequest(request)
            audioFocusRequest = null
        }
    }

    // Native methods
    private external fun initializeNative(filePath: String): Boolean
    private external fun setNativeConfig(
        usage: Int, contentType: Int, performanceMode: Int, sharingMode: Int, filePath: String
    ): Boolean
    private external fun startNativePlayback(): Boolean
    private external fun stopNativePlayback(): Boolean
    private external fun releaseNative()

    // 原生层回调
    @Suppress("unused")
    private fun onNativePlaybackStarted() {
        state = State.PLAYING
        listener?.onStarted()
        Log.i(TAG, "Playback started successfully")
    }

    @Suppress("unused")
    private fun onNativePlaybackStopped() {
        state = State.IDLE
        listener?.onStopped()
        Log.i(TAG, "Playback stopped")
    }

    @Suppress("unused")
    private fun onNativePlaybackError(error: String) {
        state = State.ERROR
        abandonAudioFocus()
        listener?.onError(error)
        Log.e(TAG, "Playback error: $error")
    }
}
