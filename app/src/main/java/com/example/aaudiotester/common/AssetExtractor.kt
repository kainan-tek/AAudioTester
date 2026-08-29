package com.example.aaudiotester.common

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Extracts an asset:// path into the app's private directory and returns a real path usable by native code.
 */
object AssetExtractor {

    private const val TAG = "AssetExtractor"
    private const val PREFIX = "asset://"

    /** Pure function: asset://x → x; plain paths are returned unchanged. */
    fun assetRelativePath(path: String): String =
        if (path.startsWith(PREFIX)) path.removePrefix(PREFIX) else path

    /** Reuses the extracted file if present, otherwise extracts from assets; plain paths are returned unchanged. */
    fun resolveAssetPath(context: Context, path: String): String {
        if (!path.startsWith(PREFIX)) return path
        val assetPath = assetRelativePath(path)
        val target = File(context.filesDir, assetPath)
        if (!target.exists()) {
            target.parentFile?.mkdirs()
            // Write to a temp file first, then atomically rename: if killed midway, no truncated file is left for exists() to mistake as complete
            val tmp = File(context.filesDir, "$assetPath.tmp")
            context.assets.open(assetPath).use { input ->
                tmp.outputStream().use { output -> input.copyTo(output) }
            }
            if (!tmp.renameTo(target)) tmp.delete() // rare cases like the target already existing: remove leftovers and reuse the existing target
            Log.i(TAG, "Extracted asset $assetPath -> ${target.absolutePath}")
        }
        return target.absolutePath
    }
}
