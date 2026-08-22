package com.example.aaudiotester.common

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

class AAudioConfigTest {

    private val xml = """
        <!-- 播放/录音配置（XML 原生注释） -->
        <aaudioConfigs>
          <player>
            <config>
              <usage>AAUDIO_USAGE_MEDIA</usage>
              <contentType>AAUDIO_CONTENT_TYPE_MUSIC</contentType>
              <audioFilePath>asset://sample/48k_2ch_16bit.wav</audioFilePath>
              <description>Media Playback</description>
            </config>
          </player>
          <recorder>
            <config>
              <inputPreset>AAUDIO_INPUT_PRESET_CAMCORDER</inputPreset>
              <sampleRate>44100</sampleRate>
              <channelCount>2</channelCount>
              <format>24</format>
              <description>Camcorder</description>
            </config>
          </recorder>
        </aaudioConfigs>
    """.trimIndent()

    private fun stream(text: String) = ByteArrayInputStream(text.toByteArray())

    @Test
    fun parseConfigs_player_section() {
        val configs = AAudioConfig.parseConfigs(stream(xml), "player")
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
        val configs = AAudioConfig.parseConfigs(stream(xml), "recorder")
        assertEquals(1, configs.size)
        val c = configs[0]
        assertEquals("AAUDIO_INPUT_PRESET_CAMCORDER", c.inputPreset)
        assertEquals(44100, c.sampleRate)
        assertEquals(2, c.channelCount)
        assertEquals(24, c.format)
    }

    @Test
    fun parseConfigs_missingSection_returnsDefault() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><player/></aaudioConfigs>"""), "recorder"
        )
        assertEquals(1, configs.size)
        assertEquals("AAUDIO_INPUT_PRESET_GENERIC", configs[0].inputPreset)
    }

    @Test
    fun parseConfigs_emptySection_returnsEmptyList() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><player/><recorder/></aaudioConfigs>"""), "recorder"
        )
        assertTrue(configs.isEmpty())
    }

    @Test
    fun parseConfigs_emptyPath_allowedForRecorder() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><recorder><config><description>auto</description></config></recorder></aaudioConfigs>"""),
            "recorder"
        )
        assertEquals("", configs[0].audioFilePath)
    }

    @Test
    fun parseConfigs_missingFields_useDefaults() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><player><config><usage>AAUDIO_USAGE_GAME</usage></config></player></aaudioConfigs>"""),
            "player"
        )
        val c = configs[0]
        assertEquals("AAUDIO_USAGE_GAME", c.usage)
        assertEquals("AAUDIO_CONTENT_TYPE_MUSIC", c.contentType)  // 省略 → 默认
        assertEquals(48000, c.sampleRate)
        assertEquals("Custom Configuration", c.description)
    }

    @Test
    fun parseConfigs_invalidNumberValue_fallsBackToDefault() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><player><config><sampleRate>abc</sampleRate></config></player></aaudioConfigs>"""),
            "player"
        )
        assertEquals(48000, configs[0].sampleRate)
    }

    @Test(expected = Exception::class)
    fun parseConfigs_malformedXml_throws_forLoadConfigsFallback() {
        // 解析异常由 loadConfigs 的 catch 兜底为 emergency 默认（此处验证契约：确实抛出）
        AAudioConfig.parseConfigs(stream("not xml"), "player")
    }

    @Test
    fun realAssetsFile_parsesBothSections() {
        // 直接解析源码树中的真实资产，防 XML 笔误只在上设备后才暴露
        val file = File("src/main/assets/aaudio_configs.xml")
        val player = file.inputStream().use { AAudioConfig.parseConfigs(it, "player") }
        val recorder = file.inputStream().use { AAudioConfig.parseConfigs(it, "recorder") }
        assertEquals(16, player.size)
        assertEquals(8, recorder.size)
        assertEquals("AAUDIO_USAGE_MEDIA", player[0].usage)
    }
}
