package com.example.aaudiotester.recorder

import android.Manifest
import android.content.Context
import com.example.aaudiotester.common.AAudioConfig
import com.example.aaudiotester.common.AAudioEngine
import com.example.aaudiotester.common.AAudioMessages
import com.example.aaudiotester.common.AAudioTestFragment
import java.util.concurrent.Executor

class RecorderFragment : AAudioTestFragment() {

    override val section: String = "recorder"
    override val messages = AAudioMessages(
        ready = "Ready to record",
        preparing = "Preparing to record...",
        active = "Recording...",
        stopped = "Recording stopped",
        failed = "Recording failed",
    )

    override fun createEngine(context: Context, engineExecutor: Executor): AAudioEngine =
        AAudioRecorder(context, engineExecutor)

    override fun requiredPermissions(): Array<String> = arrayOf(Manifest.permission.RECORD_AUDIO)

    override fun formatInfo(config: AAudioConfig): String {
        val fileDisplay = config.audioFilePath.ifBlank { "<App default path (auto-generated)>" }
        return "Current Config: ${config.description}\n" +
            "Source: ${config.inputPreset}\n" +
            "Mode: ${config.performanceMode} | ${config.sharingMode}\n" +
            "File: $fileDisplay"
    }

    override fun friendlyErrorMessage(raw: String): String = when {
        // Must precede the generic [FILE] branch, whose prefix would otherwise swallow this
        raw.startsWith("[FILE] Recording incomplete", ignoreCase = true) ->
            "The recording is incomplete: saving stopped early (storage error or 4GB WAV limit). The saved part is a valid WAV file."
        raw.startsWith("[FILE]", ignoreCase = true) ->
            "Unable to create recording file. Please check storage permissions and available space."
        raw.startsWith("[STREAM]", ignoreCase = true) ->
            "Audio system initialization failed. Please try again."
        raw.startsWith("[PARAM]", ignoreCase = true) ->
            "Invalid audio configuration. Please select a different configuration."
        raw.contains("Already recording", ignoreCase = true) -> "Recording is already in progress."
        else -> "Recording failed. Please try again."
    }
}
