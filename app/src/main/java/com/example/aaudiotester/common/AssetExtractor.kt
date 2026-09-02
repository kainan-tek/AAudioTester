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

    /**
     * Extracts an asset:// path into the app's private directory and returns a real path usable by
     * native code. The extracted copy is reused while its size still matches the APK asset: a size
     * mismatch catches an app update shipping a changed same-name asset (a stale test signal would
     * silently invalidate measurements), at zero content I/O on the hot path. Plain paths unchanged.
     * @Synchronized: callable from two fragments' executors — serialize to keep the tmp file and
     * rename collision-free.
     */
    @Synchronized
    fun resolveAssetPath(context: Context, path: String): String {
        if (!path.startsWith(PREFIX)) return path
        val assetPath = assetRelativePath(path)
        val target = File(context.filesDir, assetPath)
        // openFd fails on compressed assets: treat as "size unknown" and re-extract (safe fallback;
        // .wav is in aapt2's default no-compress list, so the metadata read is the normal path)
        val assetSize = runCatching { context.assets.openFd(assetPath).use { it.length } }.getOrNull()
        if (assetSize == null || assetSize != target.length()) {
            target.parentFile?.mkdirs()
            // Write to a temp file first, then atomically rename: playback may be reading the target
            // file while we replace it, and the rename swaps in the fresh copy as one indivisible step
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
