package com.example.aaudiotester.common

import android.media.AudioAttributes

/**
 * 合并后的 AAudio 常量（播放域 + 录音域）。
 */
object AAudioConstants {

    // 配置文件
    const val CONFIG_FILE_PATH = "/data/aaudio_configs.json"
    const val ASSETS_CONFIG_FILE = "aaudio_configs.json"

    // 内置音源
    const val DEFAULT_ASSET = "asset://sample/48k_2ch_16bit.wav"

    // 位深
    const val FORMAT_16_BIT = 16
    const val FORMAT_24_BIT = 24
    const val FORMAT_32_BIT = 32

    // 校验范围
    const val MIN_SAMPLE_RATE = 8000
    const val MAX_SAMPLE_RATE = 192000
    const val MIN_CHANNEL_COUNT = 1
    const val MAX_CHANNEL_COUNT = 16

    /** 错误类型前缀（FILE/STREAM 仅用于 native 层）。 */
    object ErrorTypes {
        const val PARAM = "[PARAM]"
        const val FOCUS = "[FOCUS]"
    }

    /** AAudio 原生常量（与 NDK 定义一致）。 */
    object AAudio {
        const val PERFORMANCE_MODE_NONE = 10
        const val PERFORMANCE_MODE_POWER_SAVING = 11
        const val PERFORMANCE_MODE_LOW_LATENCY = 12

        const val SHARING_MODE_EXCLUSIVE = 0
        const val SHARING_MODE_SHARED = 1

        const val FORMAT_PCM_I16 = 1
        const val FORMAT_PCM_I24_PACKED = 3
        const val FORMAT_PCM_I32 = 4

        const val INPUT_PRESET_GENERIC = 1
        const val INPUT_PRESET_CAMCORDER = 5
        const val INPUT_PRESET_VOICE_RECOGNITION = 6
        const val INPUT_PRESET_VOICE_COMMUNICATION = 7
        const val INPUT_PRESET_UNPROCESSED = 9
        const val INPUT_PRESET_VOICE_PERFORMANCE = 10
        const val INPUT_PRESET_SYSTEM_ECHO_REFERENCE = 1997
        const val INPUT_PRESET_SYSTEM_HOTWORD = 1999
    }

    object Usage {
        val MAP = mapOf(
            AudioAttributes.USAGE_MEDIA to "AAUDIO_USAGE_MEDIA",
            AudioAttributes.USAGE_VOICE_COMMUNICATION to "AAUDIO_USAGE_VOICE_COMMUNICATION",
            AudioAttributes.USAGE_VOICE_COMMUNICATION_SIGNALLING to "AAUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING",
            AudioAttributes.USAGE_ALARM to "AAUDIO_USAGE_ALARM",
            AudioAttributes.USAGE_NOTIFICATION to "AAUDIO_USAGE_NOTIFICATION",
            AudioAttributes.USAGE_NOTIFICATION_RINGTONE to "AAUDIO_USAGE_NOTIFICATION_RINGTONE",
            AudioAttributes.USAGE_NOTIFICATION_EVENT to "AAUDIO_USAGE_NOTIFICATION_EVENT",
            AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY to "AAUDIO_USAGE_ASSISTANCE_ACCESSIBILITY",
            AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE to "AAUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE",
            AudioAttributes.USAGE_ASSISTANCE_SONIFICATION to "AAUDIO_USAGE_ASSISTANCE_SONIFICATION",
            AudioAttributes.USAGE_GAME to "AAUDIO_USAGE_GAME",
            AudioAttributes.USAGE_ASSISTANT to "AAUDIO_USAGE_ASSISTANT",
            // AAOS（对齐 NDK 的 AAUDIO_SYSTEM_USAGE_*，需 MODIFY_AUDIO_ROUTING 特权）
            // 注：AudioAttributes.USAGE_EMERGENCY 等为 @hide 常量（SDK 不可见），故用字面量 1000-1003
            1000 to "AAUDIO_SYSTEM_USAGE_EMERGENCY",
            1001 to "AAUDIO_SYSTEM_USAGE_SAFETY",
            1002 to "AAUDIO_SYSTEM_USAGE_VEHICLE_STATUS",
            1003 to "AAUDIO_SYSTEM_USAGE_ANNOUNCEMENT"
        )
    }

    object ContentType {
        val MAP = mapOf(
            AudioAttributes.CONTENT_TYPE_SPEECH to "AAUDIO_CONTENT_TYPE_SPEECH",
            AudioAttributes.CONTENT_TYPE_MUSIC to "AAUDIO_CONTENT_TYPE_MUSIC",
            AudioAttributes.CONTENT_TYPE_MOVIE to "AAUDIO_CONTENT_TYPE_MOVIE",
            AudioAttributes.CONTENT_TYPE_SONIFICATION to "AAUDIO_CONTENT_TYPE_SONIFICATION"
        )
    }

    object InputPreset {
        val MAP = mapOf(
            AAudio.INPUT_PRESET_GENERIC to "AAUDIO_INPUT_PRESET_GENERIC",
            AAudio.INPUT_PRESET_CAMCORDER to "AAUDIO_INPUT_PRESET_CAMCORDER",
            AAudio.INPUT_PRESET_VOICE_RECOGNITION to "AAUDIO_INPUT_PRESET_VOICE_RECOGNITION",
            AAudio.INPUT_PRESET_VOICE_COMMUNICATION to "AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION",
            AAudio.INPUT_PRESET_UNPROCESSED to "AAUDIO_INPUT_PRESET_UNPROCESSED",
            AAudio.INPUT_PRESET_VOICE_PERFORMANCE to "AAUDIO_INPUT_PRESET_VOICE_PERFORMANCE",
            AAudio.INPUT_PRESET_SYSTEM_ECHO_REFERENCE to "AAUDIO_INPUT_PRESET_SYSTEM_ECHO_REFERENCE",
            AAudio.INPUT_PRESET_SYSTEM_HOTWORD to "AAUDIO_INPUT_PRESET_SYSTEM_HOTWORD"
        )
    }

    object PerformanceMode {
        val MAP = mapOf(
            AAudio.PERFORMANCE_MODE_NONE to "AAUDIO_PERFORMANCE_MODE_NONE",
            AAudio.PERFORMANCE_MODE_POWER_SAVING to "AAUDIO_PERFORMANCE_MODE_POWER_SAVING",
            AAudio.PERFORMANCE_MODE_LOW_LATENCY to "AAUDIO_PERFORMANCE_MODE_LOW_LATENCY"
        )
    }

    object SharingMode {
        val MAP = mapOf(
            AAudio.SHARING_MODE_EXCLUSIVE to "AAUDIO_SHARING_MODE_EXCLUSIVE",
            AAudio.SHARING_MODE_SHARED to "AAUDIO_SHARING_MODE_SHARED"
        )
    }

    private fun parseEnumValue(
        map: Map<Int, String>, value: String, default: Int, typeName: String = ""
    ): Int = map.entries.find { it.value == value }?.key ?: run {
        if (value.isNotEmpty()) {
            android.util.Log.w("AAudioConstants", "Unknown $typeName value: $value, using default: $default")
        }
        default
    }

    fun getUsage(usage: String): Int = parseEnumValue(Usage.MAP, usage, AudioAttributes.USAGE_MEDIA, "Usage")

    fun getContentType(contentType: String): Int = parseEnumValue(
        ContentType.MAP, contentType, AudioAttributes.CONTENT_TYPE_MUSIC, "ContentType"
    )

    fun getInputPreset(inputPreset: String): Int = parseEnumValue(
        InputPreset.MAP, inputPreset, AAudio.INPUT_PRESET_GENERIC, "InputPreset"
    )

    fun getPerformanceMode(performanceMode: String): Int = parseEnumValue(
        PerformanceMode.MAP, performanceMode, AAudio.PERFORMANCE_MODE_LOW_LATENCY, "PerformanceMode"
    )

    fun getSharingMode(sharingMode: String): Int = parseEnumValue(
        SharingMode.MAP, sharingMode, AAudio.SHARING_MODE_SHARED, "SharingMode"
    )

    fun getFormatFromBitDepth(bitDepth: Int): Int = when (bitDepth) {
        16 -> AAudio.FORMAT_PCM_I16
        24 -> AAudio.FORMAT_PCM_I24_PACKED
        32 -> AAudio.FORMAT_PCM_I32
        else -> AAudio.FORMAT_PCM_I16
    }

    fun isValidSampleRate(sampleRate: Int): Boolean = sampleRate in MIN_SAMPLE_RATE..MAX_SAMPLE_RATE

    fun isValidChannelCount(channelCount: Int): Boolean = channelCount in MIN_CHANNEL_COUNT..MAX_CHANNEL_COUNT

    fun isValidFormat(bitDepth: Int): Boolean = bitDepth in listOf(FORMAT_16_BIT, FORMAT_24_BIT, FORMAT_32_BIT)
}
