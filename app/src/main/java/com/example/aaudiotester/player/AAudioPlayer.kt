package com.example.aaudiotester.player

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.util.Log
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioConstants
import com.example.aaudiotester.common.AAudioEngineBase
import com.example.aaudiotester.common.AssetExtractor
import java.util.concurrent.Executor

/** Playback engine: AAudio output + audio focus management (session protocol shared in AAudioEngineBase). */
class AAudioPlayer(context: Context, nativeExecutor: Executor) :
    AAudioEngineBase(nativeExecutor) {

    companion object {
        private const val TAG = "AAudioPlayer"
        init {
            // Fail fast with UnsatisfiedLinkError at load time (crash point == cause point); don't swallow errors and defer to external call sites
            System.loadLibrary("aaudiotester")
        }
    }

    override val logTag = TAG
    override val sessionLabel = "playback"
    override val alreadyActiveNotice = "Already playing"

    private val appContext = context.applicationContext
    private val audioManager = appContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var currentConfig: AAudioConfig = AAudioConfig()
    private var audioFocusRequest: AudioFocusRequest? = null

    private val audioFocusChangeListener = AudioManager.OnAudioFocusChangeListener { focusChange ->
        when (focusChange) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                Log.d(TAG, "Audio focus lost, stopping playback")
                // Focus callbacks arrive on the main thread: serialized with other native calls via the shared executor
                // Same Throwable guard as the Fragment's onEngineThread: a stop that throws must degrade to a
                // retryable stuck session (the user can press Stop again), not kill the process. No listener
                // notice: onError would disable the Stop button while the session is still active.
                nativeExecutor.execute {
                    try {
                        stop()
                    } catch (t: Throwable) {
                        Log.e(TAG, "Focus-loss stop failed", t)
                    }
                }
            }
        }
    }

    private fun resolvePath(config: AAudioConfig): String {
        val path = config.audioFilePath.ifBlank { AAudioConstants.DEFAULT_ASSET }
        return try {
            AssetExtractor.resolveAssetPath(appContext, path)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to resolve asset path: $path", e)
            path  // fall back to the raw path so native reports a [FILE] error through the normal error path
        }
    }

    override fun setAudioConfig(config: AAudioConfig) {
        if (isActive()) {
            Log.w(TAG, "Cannot change configuration while playing")
            return
        }
        // Native is the source of truth: a rejected config must not update currentConfig, or start
        // would use parameters that were never applied. Rejection is defensive — the UI locks the
        // spinner across the whole busy window, so no switch can be in flight; expect true here.
        val applied = setNativeConfig(
            AAudioConstants.getUsage(config.usage),
            AAudioConstants.getContentType(config.contentType),
            AAudioConstants.getPerformanceMode(config.performanceMode),
            AAudioConstants.getSharingMode(config.sharingMode),
            resolvePath(config)
        )
        if (applied) {
            currentConfig = config
        } else {
            Log.w(TAG, "Native rejected configuration, keeping previous")
        }
    }

    override fun beforeStartNative(): String? {
        // Returned error strings are logged and delivered by the base template
        val audioPath = resolvePath(currentConfig)
        if (audioPath.isBlank() || !audioPath.lowercase().endsWith(".wav")) {
            return "${AAudioConstants.ErrorTypes.PARAM} Invalid audio file path: must end with .wav"
        }
        if (!requestAudioFocus()) {
            return "${AAudioConstants.ErrorTypes.FOCUS} Unable to obtain audio focus"
        }
        return null
    }

    override fun onSessionTerminated() {
        abandonAudioFocus()
    }

    private fun requestAudioFocus(): Boolean {
        // AAOS system usages (>=1000) cannot be represented by AudioAttributes (@hide; setUsage throws IllegalArgumentException),
        // and system usages are vehicle-critical audio that doesn't rely on normal focus management, so skip the focus request.
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
        currentConfig.usage.contains("NAVIGATION") ->
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

    // Native methods (JNI binds by this class's name; the base's protocol hooks route here)
    protected external override fun initializeNative(): Boolean
    protected external override fun startNative(): Boolean
    protected external override fun stopNative(): Boolean
    protected external override fun releaseNative()

    private external fun setNativeConfig(
        usage: Int, contentType: Int, performanceMode: Int, sharingMode: Int, filePath: String
    ): Boolean

    // Native layer callbacks: JNI binds by these exact names (aaudio_common.h); forward to the shared protocol
    @Suppress("unused")
    private fun onNativePlaybackStopped() = onNativeStopped()

    @Suppress("unused")
    private fun onNativePlaybackError(error: String) = onNativeError(error)
}
