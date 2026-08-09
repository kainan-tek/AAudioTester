package com.example.aaudiotester.common

import android.media.AudioAttributes
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AAudioConstantsTest {

    @Test
    fun getUsage_knownValue() {
        assertEquals(AudioAttributes.USAGE_MEDIA, AAudioConstants.getUsage("AAUDIO_USAGE_MEDIA"))
        assertEquals(AudioAttributes.USAGE_ALARM, AAudioConstants.getUsage("AAUDIO_USAGE_ALARM"))
    }

    @Test
    fun getUsage_unknown_returnsDefault() {
        assertEquals(AudioAttributes.USAGE_MEDIA, AAudioConstants.getUsage("AAUDIO_USAGE_UNKNOWN_XYZ"))
    }

    @Test
    fun getContentType_knownValue() {
        assertEquals(AudioAttributes.CONTENT_TYPE_MUSIC, AAudioConstants.getContentType("AAUDIO_CONTENT_TYPE_MUSIC"))
    }

    @Test
    fun getInputPreset_knownValue() {
        assertEquals(AAudioConstants.AAudio.INPUT_PRESET_CAMCORDER,
            AAudioConstants.getInputPreset("AAUDIO_INPUT_PRESET_CAMCORDER"))
    }

    @Test
    fun getFormatFromBitDepth() {
        assertEquals(AAudioConstants.AAudio.FORMAT_PCM_I16, AAudioConstants.getFormatFromBitDepth(16))
        assertEquals(AAudioConstants.AAudio.FORMAT_PCM_I24_PACKED, AAudioConstants.getFormatFromBitDepth(24))
        assertEquals(AAudioConstants.AAudio.FORMAT_PCM_I32, AAudioConstants.getFormatFromBitDepth(32))
        assertEquals(AAudioConstants.AAudio.FORMAT_PCM_I16, AAudioConstants.getFormatFromBitDepth(8))
    }

    @Test
    fun validation() {
        assertTrue(AAudioConstants.isValidSampleRate(48000))
        assertFalse(AAudioConstants.isValidSampleRate(1000))
        assertTrue(AAudioConstants.isValidChannelCount(2))
        assertFalse(AAudioConstants.isValidChannelCount(17))
        assertTrue(AAudioConstants.isValidFormat(24))
        assertFalse(AAudioConstants.isValidFormat(8))
    }
}
