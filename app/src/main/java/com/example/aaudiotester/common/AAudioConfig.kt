package com.example.aaudiotester.common

import android.content.Context
import android.util.Log
import org.w3c.dom.Element
import java.io.File
import java.io.InputStream
import javax.xml.parsers.DocumentBuilderFactory

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
            ConfigLoader.loadStream(
                context, AAudioConstants.CONFIG_FILE_PATH, AAudioConstants.ASSETS_CONFIG_FILE
            ).use { parseConfigs(it, section) }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to load $section configurations", e)
            getDefaultConfigs(section)
        }

        /** 内部 seam，便于 JVM 单测。section 缺失 → 兜底默认；空 section → 空列表 */
        internal fun parseConfigs(xml: InputStream, section: String): List<AAudioConfig> {
            val sectionElement = DocumentBuilderFactory.newInstance().newDocumentBuilder()
                .parse(xml).documentElement.getElementsByTagName(section).item(0) as? Element?
                ?: return getDefaultConfigs(section)
            val entries = sectionElement.getElementsByTagName("config")
            // 单条坏配置只跳过该条，不拖垮整个 section 回退 emergency
            return (0 until entries.length).mapNotNull { i ->
                runCatching {
                    val c = entries.item(i) as Element
                    AAudioConfig(
                        usage = c.childText("usage", "AAUDIO_USAGE_MEDIA"),
                        contentType = c.childText("contentType", "AAUDIO_CONTENT_TYPE_MUSIC"),
                        inputPreset = c.childText("inputPreset", "AAUDIO_INPUT_PRESET_GENERIC"),
                        sampleRate = c.childInt("sampleRate", 48000),
                        channelCount = c.childInt("channelCount", 1),
                        format = c.childInt("format", 16),
                        performanceMode = c.childText("performanceMode", "AAUDIO_PERFORMANCE_MODE_LOW_LATENCY"),
                        sharingMode = c.childText("sharingMode", "AAUDIO_SHARING_MODE_SHARED"),
                        audioFilePath = c.childText("audioFilePath", ""),
                        description = c.childText("description", "Custom Configuration"),
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

/**
 * XML 配置流加载器：外部路径优先，否则读 assets（XML 原生支持注释，无需预处理）。
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

/** 子元素文本读取：元素缺失 → 默认值 */
private fun Element.childText(name: String, default: String): String =
    getElementsByTagName(name).item(0)?.textContent?.trim() ?: default

private fun Element.childInt(name: String, default: Int): Int =
    getElementsByTagName(name).item(0)?.textContent?.trim()?.toIntOrNull() ?: default
