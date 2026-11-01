package com.newoether.agora.ui.settings

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

/** Revokes asynchronous editor effects before the outgoing page leaves composition. */
internal class EmbeddingEditorLifecycle : AutoCloseable {
    val imports = LocalChatModelImports()
    var closed: Boolean = false
        private set
    private var connectionTest: Job? = null

    fun testConnection(
        scope: CoroutineScope,
        test: suspend () -> String?,
        onResult: (String?) -> Unit,
    ) {
        if (closed || connectionTest?.isActive == true) return
        val job = scope.launch(start = CoroutineStart.LAZY) {
            val result = test()
            currentCoroutineContext().ensureActive()
            if (!closed) onResult(result)
        }
        connectionTest = job
        job.start()
    }

    override fun close() {
        if (closed) return
        closed = true
        connectionTest?.cancel()
        imports.close()
    }
}

internal suspend fun importEmbeddingGguf(
    context: Context,
    uri: Uri,
    imports: LocalChatModelImports,
): ImportedLocalChatFile? = withContext(Dispatchers.IO) {
    val dest = File(context.filesDir, "embedding_${UUID.randomUUID()}.gguf")
    val displayName = runCatching {
        context.contentResolver.query(
            uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null,
        )?.use { cursor ->
            val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (column >= 0 && cursor.moveToFirst()) cursor.getString(column) else null
        }
    }.getOrNull().orEmpty().ifBlank { dest.name }
    val input = context.contentResolver.openInputStream(uri) ?: return@withContext null
    if (copyLocalChatGguf(input, dest, imports)) {
        ImportedLocalChatFile(dest.absolutePath, displayName)
    } else null
}
