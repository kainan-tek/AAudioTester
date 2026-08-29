package com.example.aaudiotester.common

import android.media.AudioAttributes

/**
 * 合并后的 AAudio 常量（播放域 + 录音域）。
 */
object AAudioConstants {

    // 配置文件
    const val CONFIG_FILE_PATH = "/data/aaudio_configs.xml"
    const val ASSETS_CONFIG_FILE = "aaudio_configs.xml"

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

    /** 错误类型前缀（FILE 仅用于 native 层）。 */
    object ErrorTypes {
        const val PARAM = "[PARAM]"
        const val FOCUS = "[FOCUS]"
        const val STREAM = "[STREAM]"
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
            "AAUDIO_USAGE_MEDIA" to AudioAttributes.USAGE_MEDIA,
            "AAUDIO_USAGE_VOICE_COMMUNICATION" to AudioAttributes.USAGE_VOICE_COMMUNICATION,
            "AAUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING" to AudioAttributes.USAGE_VOICE_COMMUNICATION_SIGNALLING,
            "AAUDIO_USAGE_ALARM" to AudioAttributes.USAGE_ALARM,
            "AAUDIO_USAGE_NOTIFICATION" to AudioAttributes.USAGE_NOTIFICATION,
            "AAUDIO_USAGE_NOTIFICATION_RINGTONE" to AudioAttributes.USAGE_NOTIFICATION_RINGTONE,
            "AAUDIO_USAGE_NOTIFICATION_EVENT" to AudioAttributes.USAGE_NOTIFICATION_EVENT,
            "AAUDIO_USAGE_ASSISTANCE_ACCESSIBILITY" to AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY,
            "AAUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE" to AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE,
            "AAUDIO_USAGE_ASSISTANCE_SONIFICATION" to AudioAttributes.USAGE_ASSISTANCE_SONIFICATION,
            "AAUDIO_USAGE_GAME" to AudioAttributes.USAGE_GAME,
            "AAUDIO_USAGE_ASSISTANT" to AudioAttributes.USAGE_ASSISTANT,
            // AAOS（对齐 NDK 的 AAUDIO_SYSTEM_USAGE_*，需 MODIFY_AUDIO_ROUTING 特权）
            // 注：AudioAttributes.USAGE_EMERGENCY 等为 @hide 常量（SDK 不可见），故用字面量 1000-1003
            "AAUDIO_SYSTEM_USAGE_EMERGENCY" to 1000,
            "AAUDIO_SYSTEM_USAGE_SAFETY" to 1001,
            "AAUDIO_SYSTEM_USAGE_VEHICLE_STATUS" to 1002,
            "AAUDIO_SYSTEM_USAGE_ANNOUNCEMENT" to 1003
        )
    }

    object ContentType {
        val MAP = mapOf(
            "AAUDIO_CONTENT_TYPE_SPEECH" to AudioAttributes.CONTENT_TYPE_SPEECH,
            "AAUDIO_CONTENT_TYPE_MUSIC" to AudioAttributes.CONTENT_TYPE_MUSIC,
            "AAUDIO_CONTENT_TYPE_MOVIE" to AudioAttributes.CONTENT_TYPE_MOVIE,
            "AAUDIO_CONTENT_TYPE_SONIFICATION" to AudioAttributes.CONTENT_TYPE_SONIFICATION
        )
    }

    object InputPreset {
        val MAP = mapOf(
            "AAUDIO_INPUT_PRESET_GENERIC" to AAudio.INPUT_PRESET_GENERIC,
            "AAUDIO_INPUT_PRESET_CAMCORDER" to AAudio.INPUT_PRESET_CAMCORDER,
            "AAUDIO_INPUT_PRESET_VOICE_RECOGNITION" to AAudio.INPUT_PRESET_VOICE_RECOGNITION,
            "AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION" to AAudio.INPUT_PRESET_VOICE_COMMUNICATION,
            "AAUDIO_INPUT_PRESET_UNPROCESSED" to AAudio.INPUT_PRESET_UNPROCESSED,
            "AAUDIO_INPUT_PRESET_VOICE_PERFORMANCE" to AAudio.INPUT_PRESET_VOICE_PERFORMANCE,
            "AAUDIO_INPUT_PRESET_SYSTEM_ECHO_REFERENCE" to AAudio.INPUT_PRESET_SYSTEM_ECHO_REFERENCE,
            "AAUDIO_INPUT_PRESET_SYSTEM_HOTWORD" to AAudio.INPUT_PRESET_SYSTEM_HOTWORD
        )
    }

    object PerformanceMode {
        val MAP = mapOf(
            "AAUDIO_PERFORMANCE_MODE_NONE" to AAudio.PERFORMANCE_MODE_NONE,
            "AAUDIO_PERFORMANCE_MODE_POWER_SAVING" to AAudio.PERFORMANCE_MODE_POWER_SAVING,
            "AAUDIO_PERFORMANCE_MODE_LOW_LATENCY" to AAudio.PERFORMANCE_MODE_LOW_LATENCY
        )
    }

    object SharingMode {
        val MAP = mapOf(
            "AAUDIO_SHARING_MODE_EXCLUSIVE" to AAudio.SHARING_MODE_EXCLUSIVE,
            "AAUDIO_SHARING_MODE_SHARED" to AAudio.SHARING_MODE_SHARED
        )
    }

    private fun parseEnumValue(
        map: Map<String, Int>, value: String, default: Int, typeName: String
    ): Int = map[value] ?: run {
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

    /** 位深 → native AAudio 格式；合法位深集合同源（isValidFormat 派生自它） */
    private val FORMAT_BY_BIT_DEPTH = mapOf(
        FORMAT_16_BIT to AAudio.FORMAT_PCM_I16,
        FORMAT_24_BIT to AAudio.FORMAT_PCM_I24_PACKED,
        FORMAT_32_BIT to AAudio.FORMAT_PCM_I32,
    )

    fun getFormatFromBitDepth(bitDepth: Int): Int =
        FORMAT_BY_BIT_DEPTH[bitDepth] ?: AAudio.FORMAT_PCM_I16

    fun isValidSampleRate(sampleRate: Int): Boolean = sampleRate in MIN_SAMPLE_RATE..MAX_SAMPLE_RATE

    fun isValidChannelCount(channelCount: Int): Boolean = channelCount in MIN_CHANNEL_COUNT..MAX_CHANNEL_COUNT

    fun isValidFormat(bitDepth: Int): Boolean = bitDepth in FORMAT_BY_BIT_DEPTH
}
