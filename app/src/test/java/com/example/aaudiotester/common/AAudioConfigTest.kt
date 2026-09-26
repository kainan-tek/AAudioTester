package com.example.aaudiotester.common

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

class AAudioConfigTest {

    private val xml = """
        <!-- Playback/recording config (native XML comment) -->
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
        // Recording-domain fields fall back to defaults
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
        assertEquals(24, c.bitDepth)
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
        assertEquals("AAUDIO_CONTENT_TYPE_MUSIC", c.contentType)  // omitted → default
        assertEquals(48000, c.sampleRate)
        assertEquals("Default Configuration", c.description)  // omitted → single-source DEFAULT.description
        assertEquals(0, c.bufferBursts)  // omitted → auto tier (2 bursts low latency / 4 otherwise)
    }

    @Test
    fun parseConfigs_invalidNumberValue_fallsBackToDefault() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><player><config><sampleRate>abc</sampleRate></config></player></aaudioConfigs>"""),
            "player"
        )
        assertEquals(48000, configs[0].sampleRate)
    }

    @Test
    fun parseConfigs_negativeBufferBursts_clampedToAuto() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><player><config><bufferBursts>-5</bufferBursts></config></player></aaudioConfigs>"""),
            "player"
        )
        assertEquals(0, configs[0].bufferBursts)  // negatives mean the auto tier
    }

    @Test
    fun parseConfigs_explicitBufferBursts_parsed() {
        val configs = AAudioConfig.parseConfigs(
            stream("""<aaudioConfigs><recorder><config><bufferBursts>6</bufferBursts></config></recorder></aaudioConfigs>"""),
            "recorder"
        )
        assertEquals(6, configs[0].bufferBursts)
    }

    @Test(expected = Exception::class)
    fun parseConfigs_malformedXml_throws_forLoadConfigsFallback() {
        // Parse exceptions are caught by loadConfigs and fall back to emergency defaults (this verifies the contract: it does throw)
        AAudioConfig.parseConfigs(stream("not xml"), "player")
    }

    @Test
    fun realAssetsFile_parsesBothSections() {
        // Parse the real asset from the source tree directly, so XML typos surface before hitting a device
        val file = File("src/main/assets/aaudio_configs.xml")
        val player = file.inputStream().use { AAudioConfig.parseConfigs(it, "player") }
        val recorder = file.inputStream().use { AAudioConfig.parseConfigs(it, "recorder") }
        assertEquals(16, player.size)
        assertEquals(8, recorder.size)
        assertEquals("AAUDIO_USAGE_MEDIA", player[0].usage)
        assertEquals(4, player[0].bufferBursts)  // explicit PS tier (equals the auto tier)
    }
}
