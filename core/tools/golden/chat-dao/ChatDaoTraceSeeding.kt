// Split from ChatDaoTraceDump.kt: DAO-based entity seeding.
package chat.dao.golden

import androidx.room.RoomDatabase
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.ConversationSettingsImportTransferEntity
import com.newoether.agora.data.local.ConversationSettingsTransferEntity
import com.newoether.agora.data.local.DrawerConversationRow
import com.newoether.agora.data.local.EmbeddingEntity
import com.newoether.agora.data.local.ExecutionMessageSummaryRow
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.MessageStreamCheckpoint
import com.newoether.agora.data.local.NewChatPersistEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.local.RunGraphCommit
import com.newoether.agora.data.local.TaskEntity
import com.newoether.agora.data.local.ToolRoundCommit
import com.newoether.agora.model.ChatConversation
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import java.io.File
import java.sql.Connection
import java.sql.DriverManager
import java.util.Base64
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put
import stand.room.MiniRoom

// ---- seeding -------------------------------------------------------------------------------

internal suspend fun seed(dao: ChatDao, seed: JsonObject?) {
    if (seed == null) return
    for (element in seed["conversations"] as? JsonArray ?: JsonArray(emptyList())) {
        dao.upsertConversation(conversation(element.jsonObject))
    }
    for (element in seed["runs"] as? JsonArray ?: JsonArray(emptyList())) {
        dao.upsertRun(run(element.jsonObject))
    }
    for (element in seed["messages"] as? JsonArray ?: JsonArray(emptyList())) {
        dao.upsertMessage(message(element.jsonObject))
    }
    for (element in seed["tasks"] as? JsonArray ?: JsonArray(emptyList())) {
        dao.upsertTask(task(element.jsonObject))
    }
    for (element in seed["loops"] as? JsonArray ?: JsonArray(emptyList())) {
        dao.upsertLoopRow(loop(element.jsonObject))
    }
    for (element in seed["settingsTransfers"] as? JsonArray ?: JsonArray(emptyList())) {
        dao.upsertConversationSettingsTransfer(settingsTransfer(element.jsonObject))
    }
    val importTransfer = seed["importTransfer"] as? JsonObject
    if (importTransfer != null) {
        dao.upsertConversationSettingsImportTransfer(importTransfer(importTransfer))
    }
    val persist = seed["newChatPersist"] as? JsonObject
    if (persist != null) {
        dao.upsertNewChatPersist(newChatPersist(persist))
    }
    val embeddings = seed["embeddings"] as? JsonArray
    if (embeddings != null && embeddings.isNotEmpty()) {
        dao.insertEmbeddings(embeddings.map { embedding(it.jsonObject) })
    }
}
