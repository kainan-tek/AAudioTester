package com.example.aaudiotester.common

import android.content.Context
import java.io.File

/**
 * JSONC 配置加载：外部路径优先，否则读 assets；提供注释剥离。
 */
object ConfigLoader {

    fun loadRawText(context: Context, externalPath: String, assetName: String): String {
        val externalFile = File(externalPath)
        return if (externalFile.exists()) {
            externalFile.readText()
        } else {
            context.assets.open(assetName).bufferedReader().use { it.readText() }
        }
    }

    /** 移除 // 与 /* */ 注释，字符串内部的注释文本保留。 */
    fun stripComments(json: String): String {
        val sb = StringBuilder(json.length)
        var i = 0
        var inString = false
        while (i < json.length) {
            val c = json[i]
            when {
                inString -> {
                    sb.append(c)
                    if (c == '\\' && i + 1 < json.length) {
                        sb.append(json[i + 1]); i++
                    } else if (c == '"') {
                        inString = false
                    }
                }
                c == '"' -> { inString = true; sb.append(c) }
                c == '/' && i + 1 < json.length && json[i + 1] == '/' -> {
                    while (i < json.length && json[i] != '\n') i++
                }
                c == '/' && i + 1 < json.length && json[i + 1] == '*' -> {
                    i += 2
                    while (i + 1 < json.length && !(json[i] == '*' && json[i + 1] == '/')) i++
                    i++
                }
                else -> sb.append(c)
            }
            i++
        }
        return sb.toString()
    }
}
