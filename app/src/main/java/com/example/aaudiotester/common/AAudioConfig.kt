package com.example.aaudiotester.common

import android.content.Context
import android.util.Log
import org.w3c.dom.Element
import java.io.File
import java.io.InputStream
import javax.xml.parsers.DocumentBuilderFactory

/**
 * Combined AAudio playback/recording config (field superset). Each engine interprets the fields it uses.
 */
data class AAudioConfig(
    val usage: String = "AAUDIO_USAGE_MEDIA",                       // playback domain
    val contentType: String = "AAUDIO_CONTENT_TYPE_MUSIC",
    val inputPreset: String = "AAUDIO_INPUT_PRESET_GENERIC",        // recording domain
    val sampleRate: Int = 48000,
    val channelCount: Int = 1,
    val bitDepth: Int = 16,                                          // bit depth 16/24/32
    val performanceMode: String = "AAUDIO_PERFORMANCE_MODE_LOW_LATENCY",  // common
    val sharingMode: String = "AAUDIO_SHARING_MODE_SHARED",
    val audioFilePath: String = "",                                  // empty value is interpreted by the engine
    val description: String = "Default Configuration",
) {
    companion object {
        private const val TAG = "AAudioConfig"
        /** Single source of defaults: data class defaults, parseConfigs fallbacks, and getDefaultConfigs are all based on this */
        private val DEFAULT = AAudioConfig()

        fun loadConfigs(context: Context, section: String): List<AAudioConfig> = try {
            ConfigLoader.loadStream(
                context, AAudioConstants.CONFIG_FILE_PATH, AAudioConstants.ASSETS_CONFIG_FILE
            ).use { parseConfigs(it, section) }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load $section configurations", e)
            getDefaultConfigs(section)
        }

        /** Internal seam for JVM unit tests. Missing section → fallback defaults; empty section → empty list */
        internal fun parseConfigs(xml: InputStream, section: String): List<AAudioConfig> {
            val sectionElement = DocumentBuilderFactory.newInstance().newDocumentBuilder()
                .parse(xml).documentElement.getElementsByTagName(section).item(0) as? Element?
                ?: return getDefaultConfigs(section)
            val entries = sectionElement.getElementsByTagName("config")
            // A single bad config entry is skipped without dragging the whole section back to the emergency fallback
            return (0 until entries.length).mapNotNull { i ->
                runCatching {
                    val c = entries.item(i) as Element
                    AAudioConfig(
                        usage = c.childText("usage", DEFAULT.usage),
                        contentType = c.childText("contentType", DEFAULT.contentType),
                        inputPreset = c.childText("inputPreset", DEFAULT.inputPreset),
                        sampleRate = c.childInt("sampleRate", DEFAULT.sampleRate),
                        channelCount = c.childInt("channelCount", DEFAULT.channelCount),
                        bitDepth = c.childInt("format", DEFAULT.bitDepth),
                        performanceMode = c.childText("performanceMode", DEFAULT.performanceMode),
                        sharingMode = c.childText("sharingMode", DEFAULT.sharingMode),
                        audioFilePath = c.childText("audioFilePath", DEFAULT.audioFilePath),
                        description = c.childText("description", DEFAULT.description),
                    )
                }.onFailure {
                    Log.e(TAG, "Skipping invalid $section config entry #$i", it)
                }.getOrNull()
            }
        }

        private fun getDefaultConfigs(section: String): List<AAudioConfig> {
            Log.w(TAG, "Using hardcoded emergency configuration for $section")
            return listOf(
                if (section == "player") {
                    // Remaining fields equal the data class defaults; only write the differing ones
                    AAudioConfig(
                        audioFilePath = AAudioConstants.DEFAULT_ASSET,
                        description = "Emergency Fallback - Media Playback"
                    )
                } else {
                    AAudioConfig(description = "Emergency Fallback - Mono Recording")
                }
            )
        }
    }
}

/**
 * XML config stream loader: external path takes precedence, otherwise reads from assets (XML natively supports comments, no preprocessing needed).
 */
object ConfigLoader {

    fun loadStream(context: Context, externalPath: String, assetName: String): InputStream {
        val externalFile = File(externalPath)
        return if (externalFile.exists()) {
            externalFile.inputStream()
        } else {
            context.assets.open(assetName)
        }
    }
}

/** Reads child element text: missing element → default value */
private fun Element.childText(name: String, default: String): String =
    getElementsByTagName(name).item(0)?.textContent?.trim() ?: default

private fun Element.childInt(name: String, default: Int): Int =
    getElementsByTagName(name).item(0)?.textContent?.trim()?.toIntOrNull() ?: default
