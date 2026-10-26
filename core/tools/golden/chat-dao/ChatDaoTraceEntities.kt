// Split from ChatDaoTraceDump.kt: scenario JSON to entity construction.
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

// ---- entity construction -------------------------------------------------------------------

internal fun conversation(step: JsonObject): ChatEntity = ChatEntity(
    id = step.string("id"),
    title = step.optionalString("title") ?: "",
    lastUpdated = step.optionalLong("lastUpdated") ?: 0L,
    dataChangedAt = step.optionalLong("dataChangedAt") ?: 0L,
    selectedBranchesJson = step.optionalString("selectedBranchesJson"),
    systemPromptId = step.optionalString("systemPromptId"),
    modelId = step.optionalString("modelId"),
    taskId = step.optionalString("taskId"),
    origin = step.optionalString("origin") ?: "user",
    graduated = step.optionalBoolean("graduated") ?: false,
    draftText = step.optionalString("draftText") ?: "",
    draftAttachments = step.optionalString("draftAttachments"),
    selectedRunBranchesJson = step.optionalString("selectedRunBranchesJson"),
    hasUnreadGeneration = step.optionalBoolean("hasUnreadGeneration") ?: false,
    isPinned = step.optionalBoolean("isPinned") ?: false,
)

internal fun run(step: JsonObject): RunEntity = RunEntity(
    id = step.string("id"),
    conversationId = step.string("conversationId"),
    parentRunId = step.optionalString("parentRunId"),
    status = RunStatus.valueOf(step.optionalString("status") ?: "ACTIVE"),
    activeSlot = if (step.containsKey("activeSlot")) step.optionalInt("activeSlot") else 1,
    startedAt = step.optionalLong("startedAt") ?: 0L,
    lastCheckpointAt = step.optionalLong("lastCheckpointAt") ?: 0L,
    stopRequestedAt = step.optionalLong("stopRequestedAt"),
    endedAt = step.optionalLong("endedAt"),
    endReason = step.optionalString("endReason")?.let(RunEndReason::valueOf),
    currentPass = step.optionalInt("currentPass") ?: 0,
    legacyAmbiguous = step.optionalBoolean("legacyAmbiguous") ?: false,
)

internal fun message(step: JsonObject): MessageEntity = MessageEntity(
    id = step.string("id"),
    conversationId = step.string("conversationId"),
    parentId = step.optionalString("parentId"),
    text = step.optionalString("text") ?: "",
    images = step.stringList("images"),
    thoughts = step.optionalString("thoughts"),
    thoughtTitle = step.optionalString("thoughtTitle"),
    tokenCount = step.optionalInt("tokenCount") ?: 0,
    inputTokenCount = step.optionalInt("inputTokenCount"),
    cachedInputTokenCount = step.optionalInt("cachedInputTokenCount"),
    cacheWriteInputTokenCount = step.optionalInt("cacheWriteInputTokenCount"),
    uncachedInputTokenCount = step.optionalInt("uncachedInputTokenCount"),
    outputTokenCount = step.optionalInt("outputTokenCount"),
    reasoningTokenCount = step.optionalInt("reasoningTokenCount"),
    generationDurationMs = step.optionalLong("generationDurationMs"),
    status = MessageStatus.valueOf(step.optionalString("status") ?: "SUCCESS"),
    participant = Participant.valueOf(step.optionalString("participant") ?: "MODEL"),
    timestamp = step.optionalLong("timestamp") ?: 0L,
    thoughtTimeMs = step.optionalLong("thoughtTimeMs"),
    modelName = step.optionalString("modelName"),
    toolCallJson = step.optionalString("toolCallJson"),
    attachmentMeta = step.optionalString("attachmentMeta"),
    runId = step.string("runId"),
    runSequence = step.optionalLong("runSequence") ?: -1L,
    consumedAtPass = step.optionalInt("consumedAtPass"),
    sourceJson = step.optionalString("sourceJson"),
)

internal fun checkpoint(step: JsonObject): MessageStreamCheckpoint = MessageStreamCheckpoint(
    id = step.string("id"),
    text = step.optionalString("text") ?: "",
    images = step.stringList("images"),
    thoughts = step.optionalString("thoughts"),
    thoughtTitle = step.optionalString("thoughtTitle"),
    tokenCount = step.optionalInt("tokenCount") ?: 0,
    inputTokenCount = step.optionalInt("inputTokenCount"),
    cachedInputTokenCount = step.optionalInt("cachedInputTokenCount"),
    cacheWriteInputTokenCount = step.optionalInt("cacheWriteInputTokenCount"),
    uncachedInputTokenCount = step.optionalInt("uncachedInputTokenCount"),
    outputTokenCount = step.optionalInt("outputTokenCount"),
    reasoningTokenCount = step.optionalInt("reasoningTokenCount"),
    generationDurationMs = step.optionalLong("generationDurationMs"),
    status = MessageStatus.valueOf(step.optionalString("status") ?: "SUCCESS"),
    thoughtTimeMs = step.optionalLong("thoughtTimeMs"),
    toolCallJson = step.optionalString("toolCallJson"),
)

internal fun embedding(step: JsonObject): EmbeddingEntity = EmbeddingEntity(
    id = step.optionalLong("id") ?: 0L,
    messageId = step.string("messageId"),
    modelId = step.string("modelId"),
    embedding = Base64.getDecoder().decode(step.optionalString("embeddingBase64") ?: ""),
    chunkText = step.optionalString("chunkText") ?: "",
    dimension = step.optionalInt("dimension") ?: 0,
    sourceFingerprint = step.optionalString("sourceFingerprint"),
)

internal fun task(step: JsonObject): TaskEntity = TaskEntity(
    id = step.string("id"),
    name = step.optionalString("name") ?: "",
    prompt = step.optionalString("prompt") ?: "",
    systemPrompt = step.optionalString("systemPrompt"),
    systemPromptId = step.optionalString("systemPromptId"),
    modelId = step.optionalString("modelId"),
    cronExpr = step.optionalString("cronExpr") ?: "",
    runAt = step.optionalLong("runAt"),
    nextRunAt = step.optionalLong("nextRunAt") ?: 0L,
    enabled = step.optionalBoolean("enabled") ?: true,
    createdAt = step.optionalLong("createdAt") ?: 0L,
    lastRunAt = step.optionalLong("lastRunAt"),
)

internal fun loop(step: JsonObject): LoopEntity = LoopEntity(
    conversationId = step.string("conversationId"),
    intervalMs = step.optionalLong("intervalMs") ?: 0L,
    prompt = step.optionalString("prompt"),
    nextFireAt = step.optionalLong("nextFireAt") ?: 0L,
    cycleCount = step.optionalInt("cycleCount") ?: 0,
    maxCycles = step.optionalInt("maxCycles"),
    active = step.optionalBoolean("active") ?: true,
    revision = step.optionalLong("revision") ?: 0L,
)

internal fun newChatPersist(step: JsonObject): NewChatPersistEntity = NewChatPersistEntity(
    id = step.optionalInt("id") ?: 0,
    modelId = step.optionalString("modelId"),
    systemPromptId = step.optionalString("systemPromptId"),
    conversationSettingsJson = step.optionalString("conversationSettingsJson"),
    draftText = step.optionalString("draftText") ?: "",
    draftAttachments = step.optionalString("draftAttachments"),
)

internal fun settingsTransfer(step: JsonObject): ConversationSettingsTransferEntity =
    ConversationSettingsTransferEntity(
        conversationId = step.string("conversationId"),
        settingsJson = step.optionalString("settingsJson"),
    )

internal fun importTransfer(step: JsonObject): ConversationSettingsImportTransferEntity =
    ConversationSettingsImportTransferEntity(
        id = step.optionalInt("id") ?: 0,
        transferId = step.string("transferId"),
        settingsJson = step.optionalString("settingsJson") ?: "",
        mode = step.string("mode"),
    )

