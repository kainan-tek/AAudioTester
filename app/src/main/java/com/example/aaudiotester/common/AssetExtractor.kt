package com.example.aaudiotester.common

import android.content.Context
import android.util.Log
import java.io.File

/**
 * 将 asset:// 路径解压到 App 私有目录，返回 native 可用的真实路径。
 */
object AssetExtractor {

    private const val TAG = "AssetExtractor"
    private const val PREFIX = "asset://"

    /** 纯函数：asset://x → x；普通路径原样返回。 */
    fun assetRelativePath(path: String): String =
        if (path.startsWith(PREFIX)) path.removePrefix(PREFIX) else path

    /** 已解压则复用，否则从 assets 解压；普通路径原样返回。 */
    fun resolveAssetPath(context: Context, path: String): String {
        if (!path.startsWith(PREFIX)) return path
        val assetPath = assetRelativePath(path)
        val target = File(context.filesDir, assetPath)
        if (!target.exists()) {
            target.parentFile?.mkdirs()
            // 先写临时文件再原子重命名：中途被杀不会留下截断文件被 exists() 误判为完整
            val tmp = File(context.filesDir, "$assetPath.tmp")
            context.assets.open(assetPath).use { input ->
                tmp.outputStream().use { output -> input.copyTo(output) }
            }
            if (!tmp.renameTo(target)) tmp.delete() // 目标已存在等罕见情形：清残骸，复用现有目标
            Log.i(TAG, "Extracted asset $assetPath -> ${target.absolutePath}")
        }
        return target.absolutePath
    }
}
