// Stand-local ChatDatabase composition for NativeConversationGraphImporter.kt.
//
// ChatDatabase.kt is the Android Room composition root. The importer touches four surfaces:
// chatDao() for every graph write, semanticIndexDao() (through the verbatim
// withSemanticGraphMutation extension), maintenanceDebtDao().enqueue(), and the Room transaction
// stack. This stand supplies all four over the Room-generated schema 36 database — which also
// carries the semantic and maintenance tables the mutation runs against.
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

    fun semanticIndexDao(): SemanticIndexDao = miniRoom.create(SemanticIndexDao::class.java)

    fun maintenanceDebtDao(): MaintenanceDebtDao = miniRoom.create(MaintenanceDebtDao::class.java)

    fun close() {
        connection.close()
    }

    // The import runs inside withSemanticGraphMutation's transaction: mirror Room's real
    // BEGIN/COMMIT/ROLLBACK stack (the reader stand never needed these because it only reads).
    override fun onBegin() {
        connection.createStatement().use { it.execute("BEGIN") }
    }

    override fun onCommit() {
        connection.createStatement().use { it.execute("COMMIT") }
    }

    override fun onRollback() {
        connection.createStatement().use { it.execute("ROLLBACK") }
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
