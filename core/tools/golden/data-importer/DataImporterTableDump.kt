// Split from DataImporterTraceDump.kt: SQLite table dump after an import.
package data.importer.golden

import android.content.ContentResolver
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.newoether.agora.data.ApiKeyEntry
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.DataExporter
import com.newoether.agora.data.DataImporter
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.MemoryManager
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.data.SkillManager
import com.newoether.agora.data.SystemPromptEntry
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.ConversationSettingsTransferCoordinator
import com.newoether.agora.data.repository.SettingsRepository
import com.newoether.agora.util.readFontMeta
import java.io.File
import java.nio.file.Files
import java.sql.Connection
import java.sql.DriverManager
import java.sql.ResultSet
import java.util.Base64
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
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
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

// ---- table dump --------------------------------------------------------------------------------

internal fun queryRows(
    connection: Connection,
    sql: String,
    render: (ResultSet) -> JsonElement,
): JsonArray = buildJsonArray {
    connection.createStatement().use { statement ->
        statement.executeQuery(sql).use { rows ->
            while (rows.next()) add(render(rows))
        }
    }
}

internal fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
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
            put("dataChangedAt", row.getLong(4))
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
        "SELECT id, conversationId, parentId, text, images, tokenCount, status, participant, " +
            "timestamp, modelName, attachmentMeta, runId, runSequence, consumedAtPass, " +
            "sourceJson, toolCallJson FROM messages ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentId", row.getString(3))
            put("text", row.getString(4))
            put("images", row.getString(5))
            put("tokenCount", row.getInt(6))
            put("status", row.getString(7))
            put("participant", row.getString(8))
            put("timestamp", row.getLong(9))
            put("modelName", row.getString(10))
            put("attachmentMeta", row.getString(11))
            put("runId", row.getString(12))
            put("runSequence", row.getLong(13))
            put("consumedAtPass", (row.getObject(14) as? Number)?.toInt())
            put("sourceJson", row.getString(15))
            put("toolCallJson", row.getString(16))
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
    put("embeddings", queryRows(
        connection,
        "SELECT id, messageId, modelId, chunkText, dimension FROM embeddings ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getLong(1))
            put("messageId", row.getString(2))
            put("modelId", row.getString(3))
            put("chunkText", row.getString(4))
            put("dimension", row.getInt(5))
        }
    })
    put("conversation_settings_transfer", queryRows(
        connection,
        "SELECT conversationId, settingsJson FROM conversation_settings_transfer ORDER BY conversationId",
    ) { row ->
        buildJsonObject {
            put("conversationId", row.getString(1))
            put("settingsJson", row.getString(2))
        }
    })
    put("conversation_settings_import_transfer", queryRows(
        connection,
        "SELECT id, transferId, settingsJson, mode FROM conversation_settings_import_transfer ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getInt(1))
            put("transferId", row.getString(2))
            put("settingsJson", row.getString(3))
            put("mode", row.getString(4))
        }
    })
    put("maintenance_debt", queryRows(
        connection,
        "SELECT kind, identity, state, revision, updatedAt FROM maintenance_debt ORDER BY kind, identity",
    ) { row ->
        buildJsonObject {
            put("kind", row.getString(1))
            put("identity", row.getString(2))
            put("state", row.getString(3))
            put("revision", row.getInt(4))
            put("updatedAt", row.getLong(5))
        }
    })
    put("semantic_index_ledger", queryRows(
        connection,
        "SELECT modelId, state, sourceRevision, completedRevision, reconcileRevision, updatedAt " +
            "FROM semantic_index_ledger ORDER BY modelId",
    ) { row ->
        buildJsonObject {
            put("modelId", row.getString(1))
            put("state", row.getString(2))
            put("sourceRevision", row.getInt(3))
            put("completedRevision", row.getInt(4))
            put("reconcileRevision", row.getInt(5))
            put("updatedAt", row.getLong(6))
        }
    })
}
