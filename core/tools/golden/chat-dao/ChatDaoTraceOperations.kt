// Split from ChatDaoTraceDump.kt: every ChatDao operation dispatch.
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

// ---- operations (every ChatDao member) -----------------------------------------------------

internal suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: ChatDao,
    maskClock: Boolean,
): JsonElement = when (op) {
    // reads / projections
    "getAllConversations" -> JsonArray(dao.getAllConversations().first().map(::chatConversationJson))
    "observeDrawerConversations" -> JsonArray(
        dao.observeDrawerConversations(step.int("limit")).first().map(::drawerJson),
    )
    "getDrawerConversations" -> JsonArray(
        dao.getDrawerConversations(step.stringList("ids")).map(::drawerJson),
    )
    "getExecutionsForTask" -> JsonArray(
        dao.getExecutionsForTask(step.string("taskId")).first().map { conversationJson(it, maskClock) },
    )
    "observeConversation" -> dao.observeConversation(step.string("conversationId")).first()
        ?.let { conversationJson(it, maskClock) } ?: JsonNull
    "getConversationSettingsTransfer" ->
        dao.getConversationSettingsTransfer(step.string("conversationId"))
            ?.let(::settingsTransferJson) ?: JsonNull
    "getPendingConversationSettingsTransfers" ->
        JsonArray(dao.getPendingConversationSettingsTransfers().map(::settingsTransferJson))
    "upsertConversationSettingsTransfer" -> {
        dao.upsertConversationSettingsTransfer(settingsTransfer(step))
        JsonNull
    }
    "deleteConversationSettingsTransfer" ->
        JsonPrimitive(dao.deleteConversationSettingsTransfer(step.string("conversationId")))
    "getConversationSettingsImportTransfer" ->
        dao.getConversationSettingsImportTransfer()?.let(::importTransferJson) ?: JsonNull
    "upsertConversationSettingsImportTransfer" -> {
        dao.upsertConversationSettingsImportTransfer(importTransfer(step))
        JsonNull
    }
    "deleteConversationSettingsImportTransfer" ->
        JsonPrimitive(dao.deleteConversationSettingsImportTransfer(step.string("transferId")))
    "observeMessage" -> dao.observeMessage(step.string("messageId")).first()
        ?.let(::messageJson) ?: JsonNull
    "stopStuckMessagesForConversation" ->
        JsonPrimitive(dao.stopStuckMessagesForConversation(step.string("conversationId")))

    // conversation writes
    "upsertConversation" -> {
        dao.upsertConversation(conversation(step))
        JsonNull
    }
    "updateConversationModel" -> JsonPrimitive(
        dao.updateConversationModel(
            step.string("conversationId"),
            step.optionalString("modelId"),
            step.long("at"),
        ),
    )
    "updateConversationSystemPrompt" -> JsonPrimitive(
        dao.updateConversationSystemPrompt(
            step.string("conversationId"),
            step.optionalString("promptId"),
            step.long("at"),
        ),
    )
    "touchConversationData" -> JsonPrimitive(
        dao.touchConversationData(step.string("conversationId"), step.long("at")),
    )
    "upsertLoop" -> {
        dao.upsertLoop(loop(step))
        JsonNull
    }
    "deactivateLoopIfUnchanged" -> JsonPrimitive(
        dao.deactivateLoopIfUnchanged(
            step.string("conversationId"),
            step.long("expectedRevision"),
            step.int("expectedCycleCount"),
            step.long("expectedIntervalMs"),
            step.long("expectedNextFireAt"),
            step.int("normalizedMaxCycles"),
        ),
    )
    "deleteLoop" -> {
        dao.deleteLoop(step.string("conversationId"))
        JsonNull
    }
    "updateConversationTitle" -> JsonPrimitive(
        dao.updateConversationTitle(
            step.string("conversationId"), step.string("title"), step.long("at"),
        ),
    )
    "setConversationPinned" -> JsonPrimitive(
        dao.setConversationPinned(
            step.string("conversationId"), step.boolean("pinned"), step.long("at"),
        ),
    )
    "setConversationUnreadGeneration" -> JsonPrimitive(
        dao.setConversationUnreadGeneration(
            step.string("conversationId"), step.boolean("unread"),
        ),
    )
    "replaceConversationModelReferences" -> JsonPrimitive(
        dao.replaceConversationModelReferences(
            step.string("oldModelId"), step.optionalString("newModelId"), step.long("at"),
        ),
    )
    "replaceNewChatModelReference" -> JsonPrimitive(
        dao.replaceNewChatModelReference(
            step.string("oldModelId"), step.optionalString("newModelId"),
        ),
    )
    "updateConversationTitleIfUnchanged" -> JsonPrimitive(
        dao.updateConversationTitleIfUnchanged(
            step.string("conversationId"), step.string("expectedTitle"),
            step.string("newTitle"), step.long("at"),
        ),
    )

    // message / run writes
    "upsertMessage" -> {
        dao.upsertMessage(message(step))
        JsonNull
    }
    "upsertRun" -> {
        dao.upsertRun(run(step))
        JsonNull
    }
    "getRunsForConversation" -> JsonArray(
        dao.getRunsForConversation(step.string("conversationId")).first().map(::runJson),
    )
    "getRunsForConversationSnapshot" -> JsonArray(
        dao.getRunsForConversationSnapshot(step.string("conversationId")).map(::runJson),
    )
    "updateConversationForRunAdmission" -> JsonPrimitive(
        dao.updateConversationForRunAdmission(
            step.string("conversationId"),
            step.string("selectedBranchesJson"),
            step.string("selectedRunBranchesJson"),
            step.optionalString("modelId"),
            step.long("at"),
            step.boolean("touchConversationOnAdmission"),
        ),
    )
    "deleteMessageSubtree" -> JsonPrimitive(
        dao.deleteMessageSubtree(
            step.string("conversationId"),
            step.string("rootMessageId"),
            step.stringList("staleMessageIds"),
            step.stringList("rootRunIdsToDelete"),
            step.string("selectedBranchesJson"),
            step.string("selectedRunBranchesJson"),
            step.long("at"),
        ),
    )
    "createRunWithMessages" -> runGraphCommitJson(
        dao.createRunWithMessages(
            run = run(step.fieldObject("run")),
            messages = step.objects("messages").map(::message),
            messageSelectionUpdates = step.selectionMap("messageSelectionUpdates"),
            conversationModelId = step.optionalString("conversationModelId"),
            at = step.long("at"),
            touchConversationOnAdmission = step.boolean("touchConversationOnAdmission"),
        ),
    )
    "createConversationRunWithMessages" -> runGraphCommitJson(
        dao.createConversationRunWithMessages(
            conversation = conversation(step.fieldObject("conversation")),
            run = run(step.fieldObject("run")),
            messages = step.objects("messages").map(::message),
            messageSelectionUpdates = step.selectionMap("messageSelectionUpdates"),
            conversationModelId = step.string("conversationModelId"),
            conversationSettingsJson = step.optionalString("conversationSettingsJson"),
            expectedNewChatPersist = step.fieldObjectOrNull("expectedNewChatPersist")
                ?.let(::newChatPersist),
            at = step.long("at"),
        ),
    )
    "importRunGraph" -> {
        dao.importRunGraph(
            step.objects("runs").map(::run),
            step.objects("messages").map(::message),
        )
        JsonNull
    }
    "replaceImportedConversationGraph" -> {
        dao.replaceImportedConversationGraph(
            step.objects("conversations").map(::conversation),
            step.objects("runs").map(::run),
            step.objects("messages").map(::message),
        )
        JsonNull
    }
    "createForkGraph" -> {
        dao.createForkGraph(
            conversation(step.fieldObject("conversation")),
            step.objects("runs").map(::run),
            step.objects("messages").map(::message),
            step.stringMap("sourceToForkMessageIds"),
        )
        JsonNull
    }
    "appendToolRoundToRun" -> toolRoundCommitJson(
        dao.appendToolRoundToRun(step.objects("messages").map(::message), step.int("expectedPass")),
    )
    "markRunStopping" -> JsonPrimitive(
        dao.markRunStopping(step.string("runId"), step.long("at")),
    )
    "terminalizeLiveRun" -> JsonPrimitive(
        dao.terminalizeLiveRun(
            step.string("runId"),
            RunStatus.valueOf(step.string("status")),
            RunEndReason.valueOf(step.string("reason")),
            step.long("at"),
        ),
    )
    "stopInFlightModelMessages" -> JsonPrimitive(
        dao.stopInFlightModelMessages(step.string("runId")),
    )
    "finishGeneration" -> JsonPrimitive(
        dao.finishGeneration(
            checkpoint(step.fieldObject("checkpoint")),
            step.string("conversationId"),
            step.string("runId"),
            RunStatus.valueOf(step.string("status")),
            RunEndReason.valueOf(step.string("reason")),
            step.long("at"),
            step.boolean("markConversationUnread"),
        ),
    )
    "finishStoppedGeneration" -> JsonPrimitive(
        dao.finishStoppedGeneration(
            step.objects("checkpoints").map(::checkpoint),
            step.string("conversationId"),
            step.optionalString("runId"),
            step.long("at"),
        ),
    )
    "recoverConversationRuntime" -> JsonPrimitive(
        dao.recoverConversationRuntime(step.string("conversationId"), step.long("at")),
    )
    "updateConversationMessageCheckpoint" -> JsonPrimitive(
        dao.updateConversationMessageCheckpoint(
            step.string("conversationId"),
            checkpoint(step.fieldObject("checkpoint")),
            step.long("at"),
        ),
    )
    "updateMessageCheckpoint" -> JsonPrimitive(
        dao.updateMessageCheckpoint(checkpoint(step.fieldObject("checkpoint"))),
    )
    "deleteConversation" -> {
        dao.deleteConversation(step.string("conversationId"))
        JsonNull
    }
    "deleteMessagesByConversation" -> {
        dao.deleteMessagesByConversation(step.string("conversationId"))
        JsonNull
    }
    "updateRunParent" -> JsonPrimitive(
        dao.updateRunParent(step.string("runId"), step.optionalString("newParentRunId")),
    )
    "deleteEmbeddingsByConversation" -> {
        dao.deleteEmbeddingsByConversation(step.string("conversationId"))
        JsonNull
    }
    "deleteOrphanedEmbeddings" -> {
        dao.deleteOrphanedEmbeddings()
        JsonNull
    }
    "getLastMessageForConversation" ->
        dao.getLastMessageForConversation(step.string("conversationId"))
            ?.let(::messageJson) ?: JsonNull
    "observeExecutionMessagesForTask" -> JsonArray(
        dao.observeExecutionMessagesForTask(step.string("taskId")).first()
            .map(::executionMessageJson),
    )

    // embeddings
    "insertEmbeddings" -> JsonArray(
        dao.insertEmbeddings(step.objects("embeddings").map(::embedding)).map { JsonPrimitive(it) },
    )
    "getEmbeddingsByMessageIds" -> JsonArray(
        dao.getEmbeddingsByMessageIds(step.stringList("messageIds")).map(::embeddingJson),
    )
    "getEmbedding" -> dao.getEmbedding(step.string("messageId"))
        ?.let(::embeddingJson) ?: JsonNull
    "getAllEmbeddings" -> JsonArray(dao.getAllEmbeddings().map(::embeddingJson))

    // bulk
    "getMessagesByIds" -> JsonArray(
        dao.getMessagesByIds(step.stringList("ids")).map(::messageJson),
    )
    "updateDraft" -> {
        dao.updateDraft(
            step.string("id"), step.string("text"), step.optionalString("attachments"),
            step.long("at"),
        )
        JsonNull
    }
    "getAllConversationsList" ->
        JsonArray(dao.getAllConversationsList().map { conversationJson(it, maskClock) })
    "compareAndSetRunBranchSelections" -> JsonPrimitive(
        dao.compareAndSetRunBranchSelections(
            step.string("conversationId"), step.string("expected"), step.string("replacement"),
        ),
    )
    "getAllConversationIds" -> JsonArray(dao.getAllConversationIds().map { JsonPrimitive(it) })
    "getAllTaskIds" -> JsonArray(dao.getAllTaskIds().map { JsonPrimitive(it) })
    "getMessagesPage" -> JsonArray(
        dao.getMessagesPage(step.optionalString("afterId"), step.int("limit")).map(::messageJson),
    )
    "getConversationMessagesPage" -> JsonArray(
        dao.getConversationMessagesPage(
            step.string("conversationId"), step.optionalString("afterId"), step.int("limit"),
        ).map(::messageJson),
    )
    "deleteAllConversations" -> {
        dao.deleteAllConversations()
        JsonNull
    }
    "findExistingMessageIds" -> JsonArray(
        dao.findExistingMessageIds(step.stringList("ids")).map { JsonPrimitive(it) },
    )

    // model/provider rewrites
    "replaceConfiguredModelReferences" -> {
        dao.replaceConfiguredModelReferences(
            step.string("oldModelId"), step.optionalString("newModelId"),
        )
        JsonNull
    }
    "renameConversationProviderModelReferences" -> JsonPrimitive(
        dao.renameConversationProviderModelReferences(
            step.string("oldProvider"), step.string("newProvider"), step.long("at"),
        ),
    )
    "touchConversationsWithMessageProvider" -> JsonPrimitive(
        dao.touchConversationsWithMessageProvider(step.string("oldProvider"), step.long("at")),
    )
    "renameNewChatProviderModelReference" -> JsonPrimitive(
        dao.renameNewChatProviderModelReference(
            step.string("oldProvider"), step.string("newProvider"),
        ),
    )
    "renameMessageProviderModelReferences" -> JsonPrimitive(
        dao.renameMessageProviderModelReferences(
            step.string("oldProvider"), step.string("newProvider"),
        ),
    )
    "renameConfiguredProviderModelReferences" -> {
        dao.renameConfiguredProviderModelReferences(
            step.string("oldProvider"), step.string("newProvider"),
        )
        JsonNull
    }

    // inherited declarations, used where a scenario pins the composed ChatDao surface
    "getAllTasksList" -> JsonArray(dao.getAllTasksList().map(::taskJson))
    "getNewChatPersist" -> dao.getNewChatPersist()?.let(::newChatPersistJson) ?: JsonNull

    else -> error("unknown operation $op")
}
