package com.newoether.agora.data

import android.content.Context
import android.net.Uri
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.model.MessageSource
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import com.newoether.agora.model.SubagentCompletion
import io.mockk.every
import io.mockk.mockk
import java.io.File
import java.io.FileInputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class SubagentArchiveRoundTripTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val context = ApplicationProvider.getApplicationContext<Context>()
    private lateinit var database: ChatDatabase
    @Before fun setUp() {
        context.deleteDatabase(ChatDatabase.DB_NAME)
        database = ChatDatabase.build(context)
    }
    @After fun tearDown() {
        database.close()
        context.deleteDatabase(ChatDatabase.DB_NAME)
    }

    @Test fun realExporterAndIncrementalBackupKeepOwnershipButRestoredResultsStayDormant() = runBlocking {
        withContext(Dispatchers.IO) {
            val dao = database.chatDao()
            val result = SubagentCompletion("result", "completed", "Answer", "model",
                errorText = "Archived diagnostic", errorCode = "archived_code")
            dao.upsertConversation(ChatEntity("parent", "Parent", lastUpdated = 1L, dataChangedAt = 1L))
            dao.upsertConversation(ChatEntity("child", "Child", lastUpdated = 1L, dataChangedAt = 1L,
                parentConversationId = "parent", createdAt = 42L, subagentResultJson = SubagentCompletion.encode(result),
                subagentResultId = result.id, subagentResultStatus = result.status))
            dao.upsertConversation(ChatEntity("no-result", "Interrupted before completion", lastUpdated = 1L,
                dataChangedAt = 1L, parentConversationId = "parent", subagentResultStatus = "failed"))
            val receipt = MessageSource(MessageSource.Kind.SUBAGENT,
                subagentResults = mapOf("child" to result.id),
                subagentResult = MessageSource.SubagentResult("Child", result.status, result.errorText, result.errorCode))
            dao.insertRun(RunEntity(id = "parent-run", conversationId = "parent", parentRunId = null,
                status = RunStatus.COMPLETED, activeSlot = null, currentPass = 0, startedAt = 1L,
                lastCheckpointAt = 2L, endedAt = 2L, endReason = RunEndReason.MODEL_COMPLETED))
            dao.upsertMessage(MessageEntity(id = "receipt", conversationId = "parent", text = "Answer",
                participant = Participant.USER, timestamp = 1L, runId = "parent-run", runSequence = 0L,
                sourceJson = MessageSource.encode(receipt)))
            val settings = mockk<SettingsManager> {
                every { conversationSettings } returns flowOf(emptyMap())
            }
            val exporter = DataExporter(context, settings, mockk(), mockk())
            suspend fun export(name: String, baseline: File? = null): File {
                val file = File(temporaryFolder.root, name)
                exporter.export(Uri.fromFile(file), setOf(DataExporter.ExportCategory.CONVERSATIONS), false, baseline)
                assertTrue(file.length() > 0)
                return file
            }
            val first = export("first.agora")
            assertEquals(receipt, archivedReceipt(first))
            val restoredWithoutResult = restored(first).single { it.id == "no-result" }
            assertEquals("failed", restoredWithoutResult.subagentResultStatus)
            assertNull(restoredWithoutResult.subagentResultId)
            assertNull(restoredWithoutResult.subagentResultJson)
            assertNull(restoredWithoutResult.subagentDeliveredResultId)
            val child = restored(first).single { it.id == "child" }
            assertEquals("parent", child.parentConversationId)
            assertEquals(42L, child.createdAt)
            assertEquals(result, SubagentCompletion.decode(child.subagentResultJson))
            assertEquals("result", child.subagentDeliveredResultId)
            assertEquals(1, dao.setSubagentResult("child", "parent",
                SubagentCompletion.encode(SubagentCompletion("next", "stopped", "Stopped")), "next", "stopped", 2L))
            assertEquals(1, dao.compareAndSetSubagentStatus("child", "parent", "stopped", "failed", 3L))
            val incremental = export("incremental.agora", first)
            assertEquals(receipt, archivedReceipt(incremental))
            val updated = restored(incremental).single { it.id == "child" }
            assertEquals("parent", updated.parentConversationId)
            assertEquals(42L, updated.createdAt)
            assertEquals("next", updated.subagentResultId)
            assertEquals("next", updated.subagentDeliveredResultId)
            assertEquals("failed", updated.subagentResultStatus)
            assertEquals(SubagentCompletion("next", "stopped", "Stopped"),
                SubagentCompletion.decode(updated.subagentResultJson))
            assertEquals("failed", dao.getSubagentCachedStatus("child"))
        }
    }
    @Test fun legacyArchiveCreationTimeUsesRunEvidenceOrStoredUpdate() = runBlocking {
        val payload = NativeConversationPayload(
            conversations = listOf(
                NativeExportChatEntity("parent", "Parent", 900L),
                NativeExportChatEntity("child", "Child", 800L, parentConversationId = "parent"),
                NativeExportChatEntity("empty", "Empty", 30L, parentConversationId = "parent"),
            ),
            runs = listOf(NativeExportRunEntity("run", "child", startedAt = 20L, lastCheckpointAt = 21L)),
            messages = emptyList(), loops = emptyList(),
        )
        val importer = NativeConversationGraphImporter(database, database.chatDao(), Json { ignoreUnknownKeys = true },
            NativeConversationMediaRestorer(context, Json), scheduleMaintenance = {})
        val rows = Json.encodeToString(NativeConversationPayload.serializer(), payload).byteInputStream().use { stream ->
            importer.readConversationGraphHeaders(stream, DataImporter.ImportStrategy.REPLACE,
                NativeConversationMediaRestorer.RestoredMedia(emptyMap(), emptyMap(), emptyMap(), emptyList())) { it }
                .conversations.associateBy { it.id }
        }
        assertEquals(900L, rows.getValue("parent").createdAt)
        assertEquals(20L, rows.getValue("child").createdAt)
        assertEquals(30L, rows.getValue("empty").createdAt)
    }

    @Test fun archiveOwnershipRejectsMissingSelfNestedCyclicAndMismatchedResults() = runBlocking {
        val parent = ChatEntity("parent", "Parent")
        val child = ChatEntity("child", "Child", parentConversationId = "parent")
        suspend fun valid(rows: List<ChatEntity>, existing: Map<String, ChatEntity> = emptyMap()) =
            runCatching { validateSubagentArchiveOwners(rows, rows.map { it.id }.toSet() + existing.keys) { existing[it] } }.isSuccess
        assertTrue(valid(listOf(parent, child)))
        assertTrue(valid(listOf(child), mapOf(parent.id to parent)))
        assertFalse(valid(listOf(child)))
        assertFalse(valid(listOf(parent.copy(parentConversationId = "parent"))))
        assertFalse(valid(listOf(parent, child, ChatEntity("nested", "Nested", parentConversationId = "child"))))
        assertFalse(valid(listOf(parent.copy(parentConversationId = "child"), child)))
        val result = SubagentCompletion("result", "completed", "Answer")
        assertFalse(valid(listOf(parent, child.copy(subagentResultId = "result"))))
        assertFalse(valid(listOf(parent, child.copy(subagentResultId = "different", subagentResultStatus = result.status,
            subagentResultJson = SubagentCompletion.encode(result)))))
        assertTrue(valid(listOf(parent, child.copy(subagentResultId = result.id, subagentResultStatus = result.status,
            subagentResultJson = SubagentCompletion.encode(result)))))
        for (status in listOf("completed", "failed", "stopped")) {
            assertTrue(valid(listOf(parent, child.copy(subagentResultStatus = status))))
            assertTrue(valid(listOf(parent, child.copy(subagentResultId = result.id, subagentResultStatus = status,
                subagentResultJson = SubagentCompletion.encode(result)))))
        }
        for (status in listOf("running", "idle", "unknown")) {
            assertFalse(valid(listOf(parent, child.copy(subagentResultStatus = status))))
            assertFalse(valid(listOf(parent, child.copy(subagentResultId = result.id, subagentResultStatus = status,
                subagentResultJson = SubagentCompletion.encode(result)))))
        }
        assertFalse(valid(listOf(parent, child.copy(subagentResultId = result.id,
            subagentResultJson = SubagentCompletion.encode(result)))))
    }

    private fun archivedReceipt(file: File): MessageSource? =
        NativeBackupArchive.open(FileInputStream(file).channel).use { archive ->
            NativeConversationGraphSource.open(archive, NativeBackupFormat.CURRENT_VERSION, temporaryFolder.root).use { source ->
                source.open().bufferedReader().use { reader ->
                    val payload = Json { ignoreUnknownKeys = true }.decodeFromString(
                        NativeConversationPayload.serializer(), reader.readText(),
                    )
                    MessageSource.decode(payload.messages.single { it.id == "receipt" }.sourceJson)
                }
            }
        }

    private suspend fun restored(file: File): List<ChatEntity> =
        NativeBackupArchive.open(FileInputStream(file).channel).use { archive ->
            NativeConversationGraphSource.open(archive, NativeBackupFormat.CURRENT_VERSION, temporaryFolder.root).use { source ->
                val importer = NativeConversationGraphImporter(database, database.chatDao(), Json { ignoreUnknownKeys = true },
                    NativeConversationMediaRestorer(context, Json), scheduleMaintenance = {})
                source.open().use { stream ->
                    importer.readConversationGraphHeaders(stream, DataImporter.ImportStrategy.REPLACE,
                        NativeConversationMediaRestorer.RestoredMedia(emptyMap(), emptyMap(), emptyMap(), emptyList())) { it }
                        .conversations
                }
            }
        }
}
