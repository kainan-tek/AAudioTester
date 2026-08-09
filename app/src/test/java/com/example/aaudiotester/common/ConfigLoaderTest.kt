package com.example.aaudiotester.common

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ConfigLoaderTest {

    @Test
    fun stripComments_removesLineAndBlockComments() {
        val json = """
            {
              // line comment
              "a": 1, /* block */ "b": 2,
              /* multi
                 line */
              "c": 3
            }
        """.trimIndent()
        val out = ConfigLoader.stripComments(json)
        assertFalse(out.contains("line comment"))
        assertFalse(out.contains("block"))
        assertFalse(out.contains("multi"))
        assertTrue(out.contains("\"a\""))
        assertTrue(out.contains("\"b\""))
        assertTrue(out.contains("\"c\""))
    }

    @Test
    fun stripComments_preservesCommentTextInsideStrings() {
        val json = """{ "url": "http://x.com/a//b", "note": "keep /* here */" }"""
        val out = ConfigLoader.stripComments(json)
        assertTrue(out.contains("http://x.com/a//b"))
        assertTrue(out.contains("keep /* here */"))
    }

    @Test
    fun stripComments_producesValidJson() {
        val json = """
            {
              "configs": [
                { "name": "a" // trailing
                , "usage": "AAUDIO_USAGE_MEDIA" }
              ]
            }
        """.trimIndent()
        val out = ConfigLoader.stripComments(json)
        org.json.JSONObject(out)  // must not throw
        assertEquals(true, out.contains("\"name\": \"a\""))
    }
}
