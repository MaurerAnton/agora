// Split from ChatDaoTraceDump.kt: entity/commit JSON rendering.
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

// ---- rendering -----------------------------------------------------------------------------

internal fun chatConversationJson(entity: ChatConversation): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("title", entity.title)
    put("systemPromptId", entity.systemPromptId)
    put("modelId", entity.modelId)
    put("taskId", entity.taskId)
    put("origin", entity.origin)
    put("graduated", entity.graduated)
    put("hasUnreadGeneration", entity.hasUnreadGeneration)
    put("isPinned", entity.isPinned)
    put("selectedBranchesJson", entity.selectedBranchesJson)
}

internal fun drawerJson(entity: DrawerConversationRow): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("title", entity.title)
    put("hasUnreadGeneration", entity.hasUnreadGeneration)
    put("isPinned", entity.isPinned)
}

internal fun conversationJson(entity: ChatEntity, maskClock: Boolean = false): JsonElement =
    buildJsonObject {
    put("id", entity.id)
    put("title", entity.title)
    put("lastUpdated", entity.lastUpdated)
    put("dataChangedAt", if (maskClock) 0L else entity.dataChangedAt)
    put("selectedBranchesJson", entity.selectedBranchesJson)
    put("systemPromptId", entity.systemPromptId)
    put("modelId", entity.modelId)
    put("taskId", entity.taskId)
    put("origin", entity.origin)
    put("graduated", entity.graduated)
    put("draftText", entity.draftText)
    put("draftAttachments", entity.draftAttachments)
    put("selectedRunBranchesJson", entity.selectedRunBranchesJson)
    put("hasUnreadGeneration", entity.hasUnreadGeneration)
    put("isPinned", entity.isPinned)
}

internal fun runJson(entity: RunEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("conversationId", entity.conversationId)
    put("parentRunId", entity.parentRunId)
    put("status", entity.status.name)
    put("activeSlot", entity.activeSlot)
    put("startedAt", entity.startedAt)
    put("lastCheckpointAt", entity.lastCheckpointAt)
    put("stopRequestedAt", entity.stopRequestedAt)
    put("endedAt", entity.endedAt)
    put("endReason", entity.endReason?.name)
    put("currentPass", entity.currentPass)
    put("legacyAmbiguous", entity.legacyAmbiguous)
}

internal fun messageJson(entity: MessageEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("conversationId", entity.conversationId)
    put("parentId", entity.parentId)
    put("text", entity.text)
    put("images", JsonArray(entity.images.map { JsonPrimitive(it) }))
    put("thoughts", entity.thoughts)
    put("thoughtTitle", entity.thoughtTitle)
    put("tokenCount", entity.tokenCount)
    put("inputTokenCount", entity.inputTokenCount)
    put("cachedInputTokenCount", entity.cachedInputTokenCount)
    put("cacheWriteInputTokenCount", entity.cacheWriteInputTokenCount)
    put("uncachedInputTokenCount", entity.uncachedInputTokenCount)
    put("outputTokenCount", entity.outputTokenCount)
    put("reasoningTokenCount", entity.reasoningTokenCount)
    put("generationDurationMs", entity.generationDurationMs)
    put("status", entity.status.name)
    put("participant", entity.participant.name)
    put("timestamp", entity.timestamp)
    put("thoughtTimeMs", entity.thoughtTimeMs)
    put("modelName", entity.modelName)
    put("toolCallJson", entity.toolCallJson)
    put("attachmentMeta", entity.attachmentMeta)
    put("runId", entity.runId)
    put("runSequence", entity.runSequence)
    put("consumedAtPass", entity.consumedAtPass)
    put("sourceJson", entity.sourceJson)
}

internal fun embeddingJson(entity: EmbeddingEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("messageId", entity.messageId)
    put("modelId", entity.modelId)
    put("embeddingBase64", Base64.getEncoder().encodeToString(entity.embedding))
    put("chunkText", entity.chunkText)
    put("dimension", entity.dimension)
    put("sourceFingerprint", entity.sourceFingerprint)
}

internal fun settingsTransferJson(entity: ConversationSettingsTransferEntity): JsonElement =
    buildJsonObject {
        put("conversationId", entity.conversationId)
        put("settingsJson", entity.settingsJson)
    }

internal fun importTransferJson(entity: ConversationSettingsImportTransferEntity): JsonElement =
    buildJsonObject {
        put("id", entity.id)
        put("transferId", entity.transferId)
        put("settingsJson", entity.settingsJson)
        put("mode", entity.mode)
    }

internal fun executionMessageJson(entity: ExecutionMessageSummaryRow): JsonElement =
    buildJsonObject {
        put("id", entity.id)
        put("conversationId", entity.conversationId)
        put("participant", entity.participant.name)
        put("status", entity.status.name)
        put("timestamp", entity.timestamp)
        put("preview", entity.preview)
    }

internal fun taskJson(entity: TaskEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("name", entity.name)
    put("prompt", entity.prompt)
    put("systemPrompt", entity.systemPrompt)
    put("systemPromptId", entity.systemPromptId)
    put("modelId", entity.modelId)
    put("cronExpr", entity.cronExpr)
    put("runAt", entity.runAt)
    put("nextRunAt", entity.nextRunAt)
    put("enabled", entity.enabled)
    put("createdAt", entity.createdAt)
    put("lastRunAt", entity.lastRunAt)
}

internal fun newChatPersistJson(entity: NewChatPersistEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("modelId", entity.modelId)
    put("systemPromptId", entity.systemPromptId)
    put("conversationSettingsJson", entity.conversationSettingsJson)
    put("draftText", entity.draftText)
    put("draftAttachments", entity.draftAttachments)
}

internal fun runGraphCommitJson(commit: RunGraphCommit): JsonElement = buildJsonObject {
    put("messages", JsonArray(commit.messages.map(::messageJson)))
    put("messageSelections", selectionMapJson(commit.messageSelections))
    put("runSelections", selectionMapJson(commit.runSelections))
}

internal fun toolRoundCommitJson(commit: ToolRoundCommit): JsonElement = buildJsonObject {
    put("messages", JsonArray(commit.messages.map(::messageJson)))
    put("inserted", commit.inserted)
}

internal fun selectionMapJson(values: Map<String?, String>): JsonElement = buildJsonObject {
    for ((key, value) in values) put(key ?: "null", value)
}

