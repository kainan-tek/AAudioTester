package com.example.aaudiotester.recorder

import android.Manifest
import android.content.Context
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioEngine
import com.example.aaudiotester.common.AAudioMessages
import com.example.aaudiotester.common.AAudioTestFragment

class RecorderFragment : AAudioTestFragment() {

    override val section: String = "recorder"
    override val messages = AAudioMessages(
        ready = "Ready to record",
        preparing = "Preparing to record...",
        active = "Recording...",
        stopped = "Recording stopped",
        failed = "Recording failed",
    )

    override fun createEngine(context: Context): AAudioEngine = AAudioRecorder(context)

    override fun requiredPermissions(): Array<String> = arrayOf(Manifest.permission.RECORD_AUDIO)

    override fun formatInfo(config: AAudioConfig): String {
        val fileDisplay = config.audioFilePath.ifBlank { "<App default path (auto-generated)>" }
        return "Current Config: ${config.description}\n" +
            "Source: ${config.inputPreset}\n" +
            "Mode: ${config.performanceMode} | ${config.sharingMode}\n" +
            "File: $fileDisplay"
    }

    override fun friendlyErrorMessage(raw: String): String = when {
        raw.startsWith("[FILE]", ignoreCase = true) ->
            "Unable to create recording file. Please check storage permissions and available space."
        raw.startsWith("[STREAM]", ignoreCase = true) ->
            "Audio system initialization failed. Please try again."
        raw.startsWith("[PARAM]", ignoreCase = true) ->
            "Invalid audio configuration. Please select a different configuration."
        raw.contains("Already recording", ignoreCase = true) -> "Recording is already in progress."
        raw.contains("Not currently recording", ignoreCase = true) -> "No recording is in progress."
        else -> "Recording failed. Please try again."
    }
}
