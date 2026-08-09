package com.example.aaudiotester.common

/** 最小驱动接口：基类 Fragment 多态驱动播放/录音两引擎。 */
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
