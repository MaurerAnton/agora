// Stand-only double of tool/ToolImageStore.kt. The real class validates and atomically publishes
// image files through Android BitmapFactory/Context — a platform concern. ShellToolProvider only
// reads the store as a nullable dependency and calls persistBase64(data, mimeType, filePrefix);
// this double pins that call and returns a scripted attachment.
package com.newoether.agora.tool

import com.newoether.agora.model.ToolImageAttachment

class ToolImageStore {
    var result: ToolImageAttachment? = null
    var failure: String? = null
    val calls = mutableListOf<String>()

    fun reset() {
        result = null
        failure = null
        calls.clear()
    }

    fun persistBase64(
        data: String,
        mimeType: String,
        filePrefix: String = "tool",
    ): ToolImageAttachment {
        calls.add("persistBase64:${data.length}:$mimeType:$filePrefix")
        failure?.let { throw Exception(it) }
        return result ?: ToolImageAttachment(
            path = "/data/user/0/com.newoether.agora/files/tool-media/${filePrefix}_stand.png",
            mimeType = mimeType,
            sizeBytes = data.length.toLong() * 3 / 4,
            width = 64,
            height = 32,
            sha256 = "0".repeat(64),
        )
    }
}
