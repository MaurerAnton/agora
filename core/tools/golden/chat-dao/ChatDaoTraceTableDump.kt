// Split from ChatDaoTraceDump.kt: SQLite table dump.
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


internal fun dumpTables(connection: Connection, maskClock: Boolean): JsonObject = buildJsonObject {
    put("conversations", queryRows(
        connection,
        "SELECT id, title, lastUpdated, dataChangedAt, selectedBranchesJson, systemPromptId, " +
            "modelId, taskId, origin, graduated, draftText, draftAttachments, " +
            "selectedRunBranchesJson, hasUnreadGeneration, isPinned FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("title", row.getString(2))
            put("lastUpdated", row.getLong(3))
            put("dataChangedAt", if (maskClock) 0L else row.getLong(4))
            put("selectedBranchesJson", row.getString(5))
            put("systemPromptId", row.getString(6))
            put("modelId", row.getString(7))
            put("taskId", row.getString(8))
            put("origin", row.getString(9))
            put("graduated", row.getInt(10) != 0)
            put("draftText", row.getString(11))
            put("draftAttachments", row.getString(12))
            put("selectedRunBranchesJson", row.getString(13))
            put("hasUnreadGeneration", row.getInt(14) != 0)
            put("isPinned", row.getInt(15) != 0)
        }
    })
    put("runs", queryRows(
        connection,
        "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, " +
            "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous " +
            "FROM runs ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentRunId", row.getString(3))
            put("status", row.getString(4))
            put("activeSlot", (row.getObject(5) as? Number)?.toInt())
            put("startedAt", row.getLong(6))
            put("lastCheckpointAt", row.getLong(7))
            put("stopRequestedAt", (row.getObject(8) as? Number)?.toLong())
            put("endedAt", (row.getObject(9) as? Number)?.toLong())
            put("endReason", row.getString(10))
            put("currentPass", row.getInt(11))
            put("legacyAmbiguous", row.getInt(12) != 0)
        }
    })
    put("messages", queryRows(
        connection,
        "SELECT id, conversationId, parentId, text, images, thoughts, thoughtTitle, tokenCount, " +
            "inputTokenCount, cachedInputTokenCount, cacheWriteInputTokenCount, " +
            "uncachedInputTokenCount, outputTokenCount, reasoningTokenCount, generationDurationMs, " +
            "status, participant, timestamp, thoughtTimeMs, modelName, toolCallJson, " +
            "attachmentMeta, runId, runSequence, consumedAtPass, sourceJson FROM messages ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentId", row.getString(3))
            put("text", row.getString(4))
            put("images", row.getString(5))
            put("thoughts", row.getString(6))
            put("thoughtTitle", row.getString(7))
            put("tokenCount", row.getInt(8))
            put("inputTokenCount", (row.getObject(9) as? Number)?.toInt())
            put("cachedInputTokenCount", (row.getObject(10) as? Number)?.toInt())
            put("cacheWriteInputTokenCount", (row.getObject(11) as? Number)?.toInt())
            put("uncachedInputTokenCount", (row.getObject(12) as? Number)?.toInt())
            put("outputTokenCount", (row.getObject(13) as? Number)?.toInt())
            put("reasoningTokenCount", (row.getObject(14) as? Number)?.toInt())
            put("generationDurationMs", (row.getObject(15) as? Number)?.toLong())
            put("status", row.getString(16))
            put("participant", row.getString(17))
            put("timestamp", row.getLong(18))
            put("thoughtTimeMs", (row.getObject(19) as? Number)?.toLong())
            put("modelName", row.getString(20))
            put("toolCallJson", row.getString(21))
            put("attachmentMeta", row.getString(22))
            put("runId", row.getString(23))
            put("runSequence", row.getLong(24))
            put("consumedAtPass", (row.getObject(25) as? Number)?.toInt())
            put("sourceJson", row.getString(26))
        }
    })
    put("embeddings", queryRows(
        connection,
        "SELECT id, messageId, modelId, embedding, chunkText, dimension, sourceFingerprint " +
            "FROM embeddings ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getLong(1))
            put("messageId", row.getString(2))
            put("modelId", row.getString(3))
            put("embeddingBase64", Base64.getEncoder().encodeToString(row.getBytes(4)))
            put("chunkText", row.getString(5))
            put("dimension", row.getInt(6))
            put("sourceFingerprint", row.getString(7))
        }
    })
    put("tasks", queryRows(
        connection,
        "SELECT id, name, prompt, systemPrompt, systemPromptId, modelId, cronExpr, runAt, " +
            "nextRunAt, enabled, createdAt, lastRunAt FROM tasks ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("name", row.getString(2))
            put("prompt", row.getString(3))
            put("systemPrompt", row.getString(4))
            put("systemPromptId", row.getString(5))
            put("modelId", row.getString(6))
            put("cronExpr", row.getString(7))
            put("runAt", (row.getObject(8) as? Number)?.toLong())
            put("nextRunAt", row.getLong(9))
            put("enabled", row.getInt(10) != 0)
            put("createdAt", row.getLong(11))
            put("lastRunAt", (row.getObject(12) as? Number)?.toLong())
        }
    })
    put("loops", queryRows(
        connection,
        "SELECT conversationId, intervalMs, prompt, nextFireAt, cycleCount, maxCycles, active, " +
            "revision FROM loops ORDER BY conversationId",
    ) { row ->
        buildJsonObject {
            put("conversationId", row.getString(1))
            put("intervalMs", row.getLong(2))
            put("prompt", row.getString(3))
            put("nextFireAt", row.getLong(4))
            put("cycleCount", row.getInt(5))
            put("maxCycles", (row.getObject(6) as? Number)?.toInt())
            put("active", row.getInt(7) != 0)
            put("revision", row.getLong(8))
        }
    })
    put("new_chat_persist", queryRows(
        connection,
        "SELECT id, modelId, systemPromptId, conversationSettingsJson, draftText, " +
            "draftAttachments FROM new_chat_persist ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getInt(1))
            put("modelId", row.getString(2))
            put("systemPromptId", row.getString(3))
            put("conversationSettingsJson", row.getString(4))
            put("draftText", row.getString(5))
            put("draftAttachments", row.getString(6))
        }
    })
    put("conversation_settings_transfer", queryRows(
        connection,
        "SELECT conversationId, settingsJson FROM conversation_settings_transfer " +
            "ORDER BY conversationId",
    ) { row ->
        buildJsonObject {
            put("conversationId", row.getString(1))
            put("settingsJson", row.getString(2))
        }
    })
    put("conversation_settings_import_transfer", queryRows(
        connection,
        "SELECT id, transferId, settingsJson, mode FROM conversation_settings_import_transfer " +
            "ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getInt(1))
            put("transferId", row.getString(2))
            put("settingsJson", row.getString(3))
            put("mode", row.getString(4))
        }
    })
}

internal fun queryRows(
    connection: Connection,
    sql: String,
    render: (java.sql.ResultSet) -> JsonElement,
): JsonArray = buildJsonArray {
    connection.createStatement().use { statement ->
        statement.executeQuery(sql).use { rows ->
            while (rows.next()) add(render(rows))
        }
    }
}
