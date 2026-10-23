// Stand-local ChatDatabase composition for ConversationExportSnapshotReader.kt.
//
// ChatDatabase.kt is the Android Room composition root and migration chain. The reader only uses
// it to build a second connection, obtain ChatDao, borrow a reader connection, and close. This
// stand supplies those four platform operations over the Room-generated schema 36 database; the
// reader source itself is compiled verbatim.
package com.newoether.agora.data.local

import android.content.Context
import androidx.room.RoomDatabase
import androidx.room.Transactor
import java.sql.Connection
import java.sql.DriverManager
import java.util.concurrent.Executor
import stand.room.MiniRoom

class ChatDatabase private constructor(
    private val connection: Connection,
    internal val readerTransactor: Transactor,
) : RoomDatabase() {
    private val miniRoom = MiniRoom(this, connection)

    fun chatDao(): ChatDao = miniRoom.create(ChatDao::class.java)

    fun close() {
        connection.close()
    }

    companion object {
        const val DB_NAME = "agora_db"

        fun build(
            context: Context,
            queryExecutor: Executor? = null,
            transactionExecutor: Executor? = null,
        ): ChatDatabase {
            val connection = DriverManager.getConnection(
                "jdbc:sqlite:${context.getDatabasePath(DB_NAME).path}",
            )
            connection.createStatement().use { statement ->
                statement.execute("PRAGMA foreign_keys = ON")
            }
            val transactor = object : Transactor {
                override suspend fun <T> withTransaction(
                    type: Transactor.SQLiteTransactionType,
                    block: suspend () -> T,
                ): T {
                    connection.autoCommit = false
                    try {
                        val result = block()
                        connection.commit()
                        return result
                    } catch (failure: Throwable) {
                        try {
                            connection.rollback()
                        } catch (_: Exception) {
                            // Preserve the body failure, like a failed rollback in the JVM driver.
                        }
                        throw failure
                    } finally {
                        connection.autoCommit = true
                    }
                }
            }
            return ChatDatabase(connection, transactor)
        }
    }
}
