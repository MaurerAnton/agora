package com.newoether.agora.ui.settings

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.File
import java.io.InputStream
import java.util.UUID

/** Owns only completed, unsaved copies. Closing also rejects late import completion. */
internal class LocalChatModelImports(
    private val delete: (File) -> Unit = { it.delete(); Unit },
) : AutoCloseable {
    private val owned = mutableSetOf<File>()
    private var active = true

    @Synchronized
    fun accept(file: File): Boolean {
        if (!active) return false
        owned += file
        return true
    }

    @Synchronized
    fun discard(path: String) {
        val file = File(path)
        if (owned.remove(file)) delete(file)
    }

    @Synchronized
    fun transfer(vararg paths: String) {
        paths.forEach { owned.remove(File(it)) }
    }

    @Synchronized
    override fun close() {
        active = false
        owned.forEach(delete)
        owned.clear()
    }
}

internal data class ImportedLocalChatFile(val path: String, val displayName: String)

internal suspend fun importLocalChatGguf(
    context: Context,
    uri: Uri,
    projector: Boolean,
    imports: LocalChatModelImports,
): ImportedLocalChatFile? = withContext(Dispatchers.IO) {
    val prefix = if (projector) "mmproj" else "chat_model"
    val dest = File(context.filesDir, "${prefix}_${UUID.randomUUID()}.gguf")
    val name = runCatching {
        context.contentResolver.query(
            uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null,
        )?.use { cursor ->
            val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (column >= 0 && cursor.moveToFirst()) cursor.getString(column) else null
        }
    }.getOrNull().orEmpty()
    val input = context.contentResolver.openInputStream(uri) ?: return@withContext null
    if (copyLocalChatGguf(input, dest, imports)) {
        ImportedLocalChatFile(dest.absolutePath, name)
    } else null
}

/** Copy and validation share the same cleanup boundary, including partial and cancelled copies. */
internal suspend fun copyLocalChatGguf(
    input: InputStream,
    dest: File,
    imports: LocalChatModelImports,
): Boolean {
    var accepted = false
    try {
        input.use { source ->
            dest.outputStream().use { output ->
                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                while (true) {
                    currentCoroutineContext().ensureActive()
                    val count = source.read(buffer)
                    if (count < 0) break
                    output.write(buffer, 0, count)
                }
            }
        }
        val valid = dest.inputStream().use { source ->
            source.read() == 'G'.code && source.read() == 'G'.code &&
                source.read() == 'U'.code && source.read() == 'F'.code
        }
        if (!valid) return false
        currentCoroutineContext().ensureActive()
        accepted = imports.accept(dest)
        return accepted
    } finally {
        if (!accepted) dest.delete()
    }
}
