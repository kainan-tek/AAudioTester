package com.example.aaudiotester.player

import android.content.Context
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioEngine
import com.example.aaudiotester.common.AAudioMessages
import com.example.aaudiotester.common.AAudioTestFragment

class PlayerFragment : AAudioTestFragment() {

    override val section: String = "player"
    override val messages = AAudioMessages(
        ready = "Ready to play",
        preparing = "Preparing to play...",
        active = "Playing...",
        stopped = "Playback stopped",
        failed = "Playback failed",
    )

    override fun createEngine(context: Context): AAudioEngine = AAudioPlayer(context)

    override fun requiredPermissions(): Array<String> = emptyArray()

    override fun formatInfo(config: AAudioConfig): String =
        "Current Config: ${config.description}\n" +
            "Usage: ${config.usage} | ${config.contentType}\n" +
            "Mode: ${config.performanceMode} | ${config.sharingMode}\n" +
            "File: ${config.audioFilePath.ifBlank { "<built-in asset>" }}"

    override fun friendlyErrorMessage(raw: String): String = when {
        raw.startsWith("[FILE]", ignoreCase = true) ->
            "Unable to open audio file. The file may be corrupted or inaccessible."
        raw.startsWith("[STREAM]", ignoreCase = true) ->
            "Audio system initialization failed. Please try again."
        raw.startsWith("[PARAM]", ignoreCase = true) ->
            "Invalid audio configuration. Please select a different configuration."
        raw.startsWith("[FOCUS]", ignoreCase = true) ->
            "Unable to play audio. Another app may be using the audio system."
        raw.contains("Already playing", ignoreCase = true) -> "Playback is already in progress."
        raw.contains("Not currently playing", ignoreCase = true) -> "No playback is in progress."
        else -> "Playback failed. Please try again."
    }
}
