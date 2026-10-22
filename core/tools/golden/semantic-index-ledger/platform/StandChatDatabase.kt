// Stand-local ChatDatabase surface for compiling data/local/SemanticIndexLedger.kt verbatim.
//
// The ledger orchestration only touches three collaborators of the room database:
//   * semanticIndexDao() — the real DAO interface, executed by MiniRoom,
//   * chatDao().getConversation(id).taskId — the conversation eligibility flag,
//   * chatDao().deleteEmbeddingsByConversation(id) — the embeddings of a hidden conversation.
// The stand supplies exactly those shapes. Both DAO queries are the app's own SQL text (the
// deleteEmbeddingsByConversation query is copied from ChatDao.kt; getConversation from
// ChatContextCompactDao.kt).
package com.newoether.agora.data.local

import androidx.room.Dao
import androidx.room.Entity
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.RoomDatabase
import java.sql.Connection
import stand.room.MiniRoom

@Entity(tableName = "conversations")
data class ChatEntity(
    @PrimaryKey val id: String,
    val taskId: String? = null,
)

@Dao
interface ChatDao {
    @Query("SELECT * FROM conversations WHERE id = :conversationId")
    suspend fun getConversation(conversationId: String): ChatEntity?

    @Query(
        "DELETE FROM embeddings WHERE messageId IN " +
            "(SELECT id FROM messages WHERE conversationId = :conversationId)",
    )
    suspend fun deleteEmbeddingsByConversation(conversationId: String)
}

abstract class ChatDatabase : RoomDatabase() {
    abstract fun chatDao(): ChatDao

    abstract fun semanticIndexDao(): SemanticIndexDao
}

class StandChatDatabase(
    private val connection: Connection,
) : ChatDatabase() {
    private lateinit var room: MiniRoom
    private var chatDaoProxy: ChatDao? = null
    private var semanticIndexDaoProxy: SemanticIndexDao? = null

    fun attach(room: MiniRoom) {
        this.room = room
    }

    override fun chatDao(): ChatDao =
        chatDaoProxy ?: room.create(ChatDao::class.java).also { chatDaoProxy = it }

    override fun semanticIndexDao(): SemanticIndexDao =
        semanticIndexDaoProxy ?: room.create(SemanticIndexDao::class.java)
            .also { semanticIndexDaoProxy = it }

    override fun onBegin() {
        connection.createStatement().use { it.execute("BEGIN") }
    }

    override fun onCommit() {
        connection.createStatement().use { it.execute("COMMIT") }
    }

    override fun onRollback() {
        connection.createStatement().use { it.execute("ROLLBACK") }
    }
}
