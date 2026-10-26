// Stand-local reader-connection surface for ConversationExportSnapshotReader.kt.
//
// Room's useReaderConnection / Transactor give the reader one independent connection and one
// deferred transaction. The stand supplies the same two calls over the SQLite JDBC connection;
// the reader's traversal, paging, includeBody and task-tail sequence stay in the app source.
package androidx.room

import com.newoether.agora.data.local.ChatDatabase

interface Transactor {
    enum class SQLiteTransactionType {
        DEFERRED,
        IMMEDIATE,
        EXCLUSIVE,
    }

    suspend fun <T> withTransaction(
        type: SQLiteTransactionType,
        block: suspend () -> T,
    ): T
}

suspend fun <T> RoomDatabase.useReaderConnection(block: suspend (Transactor) -> T): T {
    val database = this as? ChatDatabase
        ?: error("useReaderConnection requires the stand ChatDatabase")
    return block(database.readerTransactor)
}
