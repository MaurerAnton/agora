// Golden trace driver for data/local/ChatDao.kt.
//
// ChatDao.kt, its six inherited interfaces, ChatDaoRuntimeRecovery.kt and every entity/model file
// they reference compile VERBATIM from the app tree. Room's generated DAO is replaced by the
// stand-local MiniRoom: @Query runs on real SQLite over platform/schema.sql (the Room-generated
// schema 36 DDL), @Insert/@Upsert/@Update implement Room's algorithms, @Transaction default
// methods execute with Room's nesting, and observed queries return cold single-emission Flows.
//
// Entity seeding uses the DAO's own upserts, so every seeded row has passed the same entity
// constructors and converters the app uses. Clock-reading transactions are visible only through
// dataChangedAt; a scenario that invokes one sets "maskClock": true and both returned
// conversation rows and the table dump render that column as 0 instead of a wall-clock timestamp.
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

private val json = Json

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

private fun render(scenarioFile: File): String = runBlocking {
    val root = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val maskClock = root.optionalBoolean("maskClock") ?: false
    val connection = DriverManager.getConnection("jdbc:sqlite::memory:")
    try {
        val pragma = connection.createStatement()
        pragma.execute("PRAGMA foreign_keys = ON")
        pragma.close()
        val schema = File("platform/schema.sql").readText()
        val statements = schema.lines()
            .map { line -> line.substringBefore("--") }
            .joinToString("\n")
            .split(";")
        for (statement in statements) {
            if (statement.isBlank()) continue
            val stmt = connection.createStatement()
            stmt.execute(statement)
            stmt.close()
        }

        val database = object : RoomDatabase() {
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
        val dao = MiniRoom(database, connection).create(ChatDao::class.java)
        seed(dao, root["seed"] as? JsonObject)

        val steps = mutableListOf<JsonElement>()
        val stepList = root["steps"] as? JsonArray ?: JsonArray(emptyList())
        for (element in stepList) {
            val step = element.jsonObject
            val op = step.string("op")
            val result = try {
                executeOperation(op, step, dao, maskClock)
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", failure::class.simpleName ?: failure::class.java.name)
                    put("message", failure.message ?: "")
                }
            }
            steps.add(buildJsonObject { put("op", op); put("result", result) })
        }

        val trace = buildJsonObject {
            put("steps", JsonArray(steps))
            put("tables", dumpTables(connection, maskClock))
        }
        json.encodeToString(JsonObject.serializer(), trace)
    } finally {
        connection.close()
    }
}

// ---- seeding -------------------------------------------------------------------------------

private suspend fun seed(dao: ChatDao, seed: JsonObject?) {
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

// ---- operations (every ChatDao member) -----------------------------------------------------

private suspend fun executeOperation(
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

// ---- entity construction -------------------------------------------------------------------

private fun conversation(step: JsonObject): ChatEntity = ChatEntity(
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

private fun run(step: JsonObject): RunEntity = RunEntity(
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

private fun message(step: JsonObject): MessageEntity = MessageEntity(
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

private fun checkpoint(step: JsonObject): MessageStreamCheckpoint = MessageStreamCheckpoint(
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

private fun embedding(step: JsonObject): EmbeddingEntity = EmbeddingEntity(
    id = step.optionalLong("id") ?: 0L,
    messageId = step.string("messageId"),
    modelId = step.string("modelId"),
    embedding = Base64.getDecoder().decode(step.optionalString("embeddingBase64") ?: ""),
    chunkText = step.optionalString("chunkText") ?: "",
    dimension = step.optionalInt("dimension") ?: 0,
    sourceFingerprint = step.optionalString("sourceFingerprint"),
)

private fun task(step: JsonObject): TaskEntity = TaskEntity(
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

private fun loop(step: JsonObject): LoopEntity = LoopEntity(
    conversationId = step.string("conversationId"),
    intervalMs = step.optionalLong("intervalMs") ?: 0L,
    prompt = step.optionalString("prompt"),
    nextFireAt = step.optionalLong("nextFireAt") ?: 0L,
    cycleCount = step.optionalInt("cycleCount") ?: 0,
    maxCycles = step.optionalInt("maxCycles"),
    active = step.optionalBoolean("active") ?: true,
    revision = step.optionalLong("revision") ?: 0L,
)

private fun newChatPersist(step: JsonObject): NewChatPersistEntity = NewChatPersistEntity(
    id = step.optionalInt("id") ?: 0,
    modelId = step.optionalString("modelId"),
    systemPromptId = step.optionalString("systemPromptId"),
    conversationSettingsJson = step.optionalString("conversationSettingsJson"),
    draftText = step.optionalString("draftText") ?: "",
    draftAttachments = step.optionalString("draftAttachments"),
)

private fun settingsTransfer(step: JsonObject): ConversationSettingsTransferEntity =
    ConversationSettingsTransferEntity(
        conversationId = step.string("conversationId"),
        settingsJson = step.optionalString("settingsJson"),
    )

private fun importTransfer(step: JsonObject): ConversationSettingsImportTransferEntity =
    ConversationSettingsImportTransferEntity(
        id = step.optionalInt("id") ?: 0,
        transferId = step.string("transferId"),
        settingsJson = step.optionalString("settingsJson") ?: "",
        mode = step.string("mode"),
    )

// ---- rendering -----------------------------------------------------------------------------

private fun chatConversationJson(entity: ChatConversation): JsonElement = buildJsonObject {
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

private fun drawerJson(entity: DrawerConversationRow): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("title", entity.title)
    put("hasUnreadGeneration", entity.hasUnreadGeneration)
    put("isPinned", entity.isPinned)
}

private fun conversationJson(entity: ChatEntity, maskClock: Boolean = false): JsonElement =
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

private fun runJson(entity: RunEntity): JsonElement = buildJsonObject {
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

private fun messageJson(entity: MessageEntity): JsonElement = buildJsonObject {
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

private fun embeddingJson(entity: EmbeddingEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("messageId", entity.messageId)
    put("modelId", entity.modelId)
    put("embeddingBase64", Base64.getEncoder().encodeToString(entity.embedding))
    put("chunkText", entity.chunkText)
    put("dimension", entity.dimension)
    put("sourceFingerprint", entity.sourceFingerprint)
}

private fun settingsTransferJson(entity: ConversationSettingsTransferEntity): JsonElement =
    buildJsonObject {
        put("conversationId", entity.conversationId)
        put("settingsJson", entity.settingsJson)
    }

private fun importTransferJson(entity: ConversationSettingsImportTransferEntity): JsonElement =
    buildJsonObject {
        put("id", entity.id)
        put("transferId", entity.transferId)
        put("settingsJson", entity.settingsJson)
        put("mode", entity.mode)
    }

private fun executionMessageJson(entity: ExecutionMessageSummaryRow): JsonElement =
    buildJsonObject {
        put("id", entity.id)
        put("conversationId", entity.conversationId)
        put("participant", entity.participant.name)
        put("status", entity.status.name)
        put("timestamp", entity.timestamp)
        put("preview", entity.preview)
    }

private fun taskJson(entity: TaskEntity): JsonElement = buildJsonObject {
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

private fun newChatPersistJson(entity: NewChatPersistEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("modelId", entity.modelId)
    put("systemPromptId", entity.systemPromptId)
    put("conversationSettingsJson", entity.conversationSettingsJson)
    put("draftText", entity.draftText)
    put("draftAttachments", entity.draftAttachments)
}

private fun runGraphCommitJson(commit: RunGraphCommit): JsonElement = buildJsonObject {
    put("messages", JsonArray(commit.messages.map(::messageJson)))
    put("messageSelections", selectionMapJson(commit.messageSelections))
    put("runSelections", selectionMapJson(commit.runSelections))
}

private fun toolRoundCommitJson(commit: ToolRoundCommit): JsonElement = buildJsonObject {
    put("messages", JsonArray(commit.messages.map(::messageJson)))
    put("inserted", commit.inserted)
}

private fun selectionMapJson(values: Map<String?, String>): JsonElement = buildJsonObject {
    for ((key, value) in values) put(key ?: "null", value)
}

// ---- table dump ----------------------------------------------------------------------------

private fun dumpTables(connection: Connection, maskClock: Boolean): JsonObject = buildJsonObject {
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

private fun queryRows(
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

// ---- JSON helpers --------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

private fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

private fun JsonObject.boolean(name: String): Boolean =
    optionalString(name)?.toBooleanStrictOrNull() ?: error("missing boolean $name")

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    optionalString(name)?.toBooleanStrictOrNull()

private fun JsonObject.stringList(name: String): List<String> =
    (this[name] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList()

private fun JsonObject.fieldObject(name: String): JsonObject =
    this[name] as? JsonObject ?: error("missing object $name")

private fun JsonObject.fieldObjectOrNull(name: String): JsonObject? = this[name] as? JsonObject

private fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it.jsonObject } ?: emptyList()

private fun JsonObject.stringMap(name: String): Map<String, String> =
    (this[name] as? JsonObject)?.entries?.associate { (key, value) ->
        key to (value as JsonPrimitive).content
    } ?: emptyMap()

private fun JsonObject.selectionMap(name: String): Map<String?, String> =
    stringMap(name).mapKeys { (key, _) -> if (key == "null") null else key }
