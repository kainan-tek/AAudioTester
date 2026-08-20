package com.example.aaudiotester.common

import org.junit.Assert.assertEquals
import org.junit.Test

class AAudioConfigTest {

    private val jsonc = """
        {
          // 播放配置
          "player": [
            { "usage": "AAUDIO_USAGE_MEDIA", "contentType": "AAUDIO_CONTENT_TYPE_MUSIC",
              "audioFilePath": "asset://sample/48k_2ch_16bit.wav",
              "description": "Media Playback" }
          ],
          // 录音配置
          "recorder": [
            { "inputPreset": "AAUDIO_INPUT_PRESET_CAMCORDER", "sampleRate": 44100,
              "channelCount": 2, "format": 24, "description": "Camcorder" }
          ]
        }
    """.trimIndent()

    @Test
    fun parseConfigs_player_section() {
        val configs = AAudioConfig.parseConfigs(ConfigLoader.stripComments(jsonc), "player")
        assertEquals(1, configs.size)
        val c = configs[0]
        assertEquals("AAUDIO_USAGE_MEDIA", c.usage)
        assertEquals("AAUDIO_CONTENT_TYPE_MUSIC", c.contentType)
        assertEquals("asset://sample/48k_2ch_16bit.wav", c.audioFilePath)
        // 录音域字段走默认
        assertEquals(1, c.channelCount)
        assertEquals("AAUDIO_INPUT_PRESET_GENERIC", c.inputPreset)
    }

    @Test
    fun parseConfigs_recorder_section() {
        val configs = AAudioConfig.parseConfigs(ConfigLoader.stripComments(jsonc), "recorder")
        assertEquals(1, configs.size)
        val c = configs[0]
        assertEquals("AAUDIO_INPUT_PRESET_CAMCORDER", c.inputPreset)
        assertEquals(44100, c.sampleRate)
        assertEquals(2, c.channelCount)
        assertEquals(24, c.format)
    }

    @Test
    fun parseConfigs_missingSection_returnsDefault() {
        val configs = AAudioConfig.parseConfigs("""{ "player": [] }""", "recorder")
        assertEquals(1, configs.size)
        assertEquals("AAUDIO_INPUT_PRESET_GENERIC", configs[0].inputPreset)
    }

    @Test
    fun parseConfigs_emptyPath_allowedForRecorder() {
        val configs = AAudioConfig.parseConfigs(
            """{ "recorder": [ { "description": "auto" } ] }""", "recorder"
        )
        assertEquals("", configs[0].audioFilePath)
    }

    @Test
    fun parseConfigs_badEntry_skipsOnlyThatEntry() {
        val json = """{ "player": [ "not-an-object",
                           { "usage": "AAUDIO_USAGE_GAME", "description": "ok" } ] }"""
        val configs = AAudioConfig.parseConfigs(json, "player")
        assertEquals(1, configs.size)
        assertEquals("AAUDIO_USAGE_GAME", configs[0].usage)
    }
}
