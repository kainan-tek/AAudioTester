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
import java.util.concurrent.Executor

/** Playback engine: AAudio output + audio focus management. */
class AAudioPlayer(context: Context, private val nativeExecutor: Executor) : AAudioEngine {

    companion object {
        private const val TAG = "AAudioPlayer"
        init {
            // Fail fast with UnsatisfiedLinkError at load time (crash point == cause point); don't swallow errors and defer to external call sites
            System.loadLibrary("aaudiotester")
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

    @Volatile
    private var errored = false  // This session already reported an error: late started callbacks are ignored (error-first, terminal)

    @Volatile
    private var nativeBound = false  // initializeNative may fail on rotation rebuild if the old session hasn't stopped; retried at start

    private val audioFocusChangeListener = AudioManager.OnAudioFocusChangeListener { focusChange ->
        when (focusChange) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                Log.d(TAG, "Audio focus lost, stopping playback")
                // Focus callbacks arrive on the main thread: serialized with other native calls via the shared executor
                nativeExecutor.execute { stop() }
            }
        }
    }

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

    private fun resolvePath(config: AAudioConfig): String {
        val path = config.audioFilePath.ifBlank { AAudioConstants.DEFAULT_ASSET }
        return try {
            AssetExtractor.resolveAssetPath(appContext, path)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to resolve asset path: $path", e)
            path  // fall back to the raw path so native reports a [FILE] error through the normal error path
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

    override fun start(): Boolean {
        if (!ensureNativeBound()) {
            val error = "${AAudioConstants.ErrorTypes.STREAM} Native initialization failed"
            Log.e(TAG, error)
            listener?.onError(error)
            return false
        }
        if (state == State.PLAYING) {
            Log.w(TAG, "Already playing")
            listener?.onError("Already playing")
            return false
        }
        if (state == State.ERROR) state = State.IDLE
        errored = false  // reset for a new session

        val audioPath = resolvePath(currentConfig)
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
        if (!result) {
            abandonAudioFocus()
            // A real native failure always delivers onError synchronously (state becomes ERROR before
            // this returns). state still IDLE here means native returned false without notifying —
            // the cross-language contract is broken; recover the UI instead of sticking it forever
            if (state == State.IDLE) {
                listener?.onError("${AAudioConstants.ErrorTypes.STREAM} Native start failed without error callback")
            }
        }
        return result
    }

    override fun stop() {
        // Also entered from ERROR state: cleans up the stream/file/reader thread left by an error (triggered by Fragment.onError)
        if (state == State.IDLE) return
        Log.d(TAG, "Stopping playback")
        stopNativePlayback()
        state = State.IDLE
    }

    override fun isActive(): Boolean = state == State.PLAYING

    override fun release() {
        if (state == State.PLAYING) stop()
        abandonAudioFocus()
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
        Log.d(TAG, "AAudioPlayer resources released")
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

    // Native methods
    private external fun initializeNative(): Boolean
    private external fun setNativeConfig(
        usage: Int, contentType: Int, performanceMode: Int, sharingMode: Int, filePath: String
    ): Boolean
    private external fun startNativePlayback(): Boolean
    private external fun stopNativePlayback()
    private external fun releaseNative()

    // Native layer callbacks
    @Suppress("unused")
    private fun onNativePlaybackStarted() {
        if (errored) return  // error-first: native already stopped, ignore a started arriving late in a race
        state = State.PLAYING
        listener?.onStarted()
        Log.i(TAG, "Playback started successfully")
    }

    @Suppress("unused")
    private fun onNativePlaybackStopped() {
        state = State.IDLE
        abandonAudioFocus()
        listener?.onStopped()
        Log.i(TAG, "Playback stopped")
    }

    @Suppress("unused")
    private fun onNativePlaybackError(error: String) {
        errored = true  // terminal error state for this session
        state = State.ERROR
        abandonAudioFocus()
        listener?.onError(error)
        Log.e(TAG, "Playback error: $error")
    }
}
