package com.example.aaudiotester.common

/** Minimal driver interface: the base Fragment polymorphically drives both the playback and recording engines. */
interface AAudioEngine {
    interface Listener {
        fun onStarted()
        fun onStopped()
        fun onError(error: String)
    }

    fun setListener(listener: Listener?)
    fun setAudioConfig(config: AAudioConfig)
    fun start(): Boolean
    fun stop()
    fun isActive(): Boolean
    fun release()
}
