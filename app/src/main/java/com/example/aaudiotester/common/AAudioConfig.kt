package com.example.aaudiotester.common

import android.content.Context
import android.util.Log
import org.json.JSONObject

/**
 * AAudio 播放/录音联合配置（字段超集）。各引擎解释自己使用的字段。
 */
data class AAudioConfig(
    val usage: String = "AAUDIO_USAGE_MEDIA",                       // 播放域
    val contentType: String = "AAUDIO_CONTENT_TYPE_MUSIC",
    val inputPreset: String = "AAUDIO_INPUT_PRESET_GENERIC",        // 录音域
    val sampleRate: Int = 48000,
    val channelCount: Int = 1,
    val format: Int = 16,                                            // 位深 16/24/32
    val performanceMode: String = "AAUDIO_PERFORMANCE_MODE_LOW_LATENCY",  // 公共
    val sharingMode: String = "AAUDIO_SHARING_MODE_SHARED",
    val audioFilePath: String = "",                                  // 空值由引擎解释
    val description: String = "Default Configuration",
) {
    companion object {
        private const val TAG = "AAudioConfig"

        fun loadConfigs(context: Context, section: String): List<AAudioConfig> = try {
            val raw = ConfigLoader.loadRawText(
                context, AAudioConstants.CONFIG_FILE_PATH, AAudioConstants.ASSETS_CONFIG_FILE
            )
            parseConfigs(ConfigLoader.stripComments(raw), section)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load $section configurations", e)
            getDefaultConfigs(section)
        }

        /** 内部 seam，便于 JVM 单测。 */
        internal fun parseConfigs(jsonString: String, section: String): List<AAudioConfig> {
            val root = JSONObject(jsonString)
            if (!root.has(section)) return getDefaultConfigs(section)
            val array = root.getJSONArray(section)
            // 单条坏配置只跳过该条，不拖垮整个 section 回退 emergency
            return (0 until array.length()).mapNotNull { i ->
                runCatching {
                    val c = array.getJSONObject(i)
                    AAudioConfig(
                        usage = c.optString("usage", "AAUDIO_USAGE_MEDIA"),
                        contentType = c.optString("contentType", "AAUDIO_CONTENT_TYPE_MUSIC"),
                        inputPreset = c.optString("inputPreset", "AAUDIO_INPUT_PRESET_GENERIC"),
                        sampleRate = c.optInt("sampleRate", 48000),
                        channelCount = c.optInt("channelCount", 1),
                        format = c.optInt("format", 16),
                        performanceMode = c.optString("performanceMode", "AAUDIO_PERFORMANCE_MODE_LOW_LATENCY"),
                        sharingMode = c.optString("sharingMode", "AAUDIO_SHARING_MODE_SHARED"),
                        audioFilePath = c.optString("audioFilePath", ""),
                        description = c.optString("description", "Custom Configuration"),
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
                    AAudioConfig(
                        usage = "AAUDIO_USAGE_MEDIA",
                        contentType = "AAUDIO_CONTENT_TYPE_MUSIC",
                        audioFilePath = AAudioConstants.DEFAULT_ASSET,
                        description = "Emergency Fallback - Media Playback"
                    )
                } else {
                    AAudioConfig(
                        inputPreset = "AAUDIO_INPUT_PRESET_GENERIC",
                        sampleRate = 48000, channelCount = 1, format = 16,
                        audioFilePath = "",
                        description = "Emergency Fallback - Mono Recording"
                    )
                }
            )
        }
    }
}
