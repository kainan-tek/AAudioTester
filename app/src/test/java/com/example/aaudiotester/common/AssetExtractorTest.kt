package com.example.aaudiotester.common

import org.junit.Assert.assertEquals
import org.junit.Test

class AssetExtractorTest {

    @Test
    fun assetRelativePath_stripsPrefix() {
        assertEquals("sample/48k_2ch_16bit.wav",
            AssetExtractor.assetRelativePath("asset://sample/48k_2ch_16bit.wav"))
    }

    @Test
    fun assetRelativePath_passthroughForPlainPath() {
        assertEquals("/data/48k_2ch_16bit.wav",
            AssetExtractor.assetRelativePath("/data/48k_2ch_16bit.wav"))
    }
}
