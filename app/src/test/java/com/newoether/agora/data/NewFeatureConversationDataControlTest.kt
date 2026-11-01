package com.newoether.agora.data

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.data.DataImporter.ImportStrategy
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.semanticModelSnapshot
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.pendingSubagentResults
import com.newoether.agora.model.ConversationTodo
import com.newoether.agora.model.ConversationTodoItem
import com.newoether.agora.model.MessageSource
import com.newoether.agora.model.SubagentCompletion
import java.io.ByteArrayInputStream
import java.io.InputStream
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class NewFeatureConversationDataControlTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val context = ApplicationProvider.getApplicationContext<Context>()
    private val json = Json { ignoreUnknownKeys = true }
    private lateinit var database: ChatDatabase
    private lateinit var dao: ChatDao
    private val todo = ConversationTodo.encode(listOf(
        ConversationTodoItem("Check archive", "completed"),
        ConversationTodoItem("Keep order", "in_progress"),
        ConversationTodoItem("Next"),
    ))
    private val result = SubagentCompletion("result-child", "completed", "Answer", "model")
    private val source = MessageSource(
        MessageSource.Kind.SUBAGENT, subagentResults = mapOf("child" to result.id),
    )

    @Before fun setUp() {
        database = Room.inMemoryDatabaseBuilder(context, ChatDatabase::class.java)
            .allowMainThreadQueries().build()
        dao = database.chatDao()
    }

    @After fun tearDown() = database.close()

    @Test
    fun nativeMergeAndReplaceKeepTodoOwnershipAndReceiptsWithoutRevivingOutbox() = runTest {
        val unrelated = ChatEntity("unrelated", "Unrelated", todoJson = todo)
        dao.upsertConversation(unrelated)
        val rows = listOf(
            NativeExportChatEntity("parent", "Parent", 1L, todoJson = todo),
            NativeExportChatEntity("child", "Child", 1L, parentConversationId = "parent",
                todoJson = todo, subagentResultJson = SubagentCompletion.encode(result),
                subagentResultId = result.id, subagentResultStatus = result.status),
        )
        val messages = listOf(
            NativeExportMessageEntity("input", "parent", text = "Child result", participant = "USER",
                timestamp = 1L, runId = "run", runSequence = 0L, sourceJson = MessageSource.encode(source)),
            NativeExportMessageEntity("answer", "parent", parentId = "input", text = "Received",
                timestamp = 2L, runId = "run", runSequence = 1L),
        )
        restore(rows, messages, ImportStrategy.MERGE)
        assertEquals(unrelated, dao.getConversation(unrelated.id))
        assertEquals(todo, dao.getConversation("parent")?.todoJson)
        assertEquals(todo, dao.getConversation("child")?.todoJson)
        assertEquals("parent", dao.getConversation("child")?.parentConversationId)
        assertEquals(result, SubagentCompletion.decode(dao.getConversation("child")?.subagentResultJson))
        assertEquals(result.id, dao.getConversation("child")?.subagentDeliveredResultId)
        assertEquals(source, MessageSource.decode(dao.getMessage("input")?.sourceJson))
        assertTrue(ConversationRepository(dao, database, scheduleMaintenance = {})
            .pendingSubagentResults("parent").isEmpty())

        restore(rows, messages, ImportStrategy.REPLACE)
        assertEquals(setOf("parent", "child"), dao.getAllConversationIds().toSet())
        assertNull(dao.getConversation(unrelated.id))
        assertEquals(todo, dao.getConversation("child")?.todoJson)
        assertEquals(result.id, dao.getConversation("child")?.subagentDeliveredResultId)
        assertEquals(source, MessageSource.decode(dao.getMessage("input")?.sourceJson))
        ConversationRepository(dao, database, scheduleMaintenance = {}).deleteConversation("parent")
        assertTrue(dao.getAllConversationIds().isEmpty())
        assertNull(dao.getMessage("input"))
        assertNull(dao.getMessage("answer"))
    }

    @Test
    fun sourceImportKeepsOnlyCanonicalUserReceiptsAndLegacyRowsHaveNone() = runTest {
        val encoded = MessageSource.encode(source)
        val rows = listOf(NativeExportChatEntity("parent", "Parent", 1L))
        val messages = listOf(
            NativeExportMessageEntity("input", "parent", text = "Automatic", participant = "USER",
                timestamp = 1L, runId = "run", runSequence = 0L, sourceJson = encoded),
            NativeExportMessageEntity("answer", "parent", parentId = "input", text = "Model",
                timestamp = 2L, runId = "run", runSequence = 1L, sourceJson = encoded),
            NativeExportMessageEntity("malformed", "parent", parentId = "answer", text = "Keep text",
                participant = "USER", timestamp = 3L, runId = "run", runSequence = 2L,
                sourceJson = "{broken"),
            NativeExportMessageEntity("legacy", "parent", parentId = "malformed", text = "Legacy",
                timestamp = 4L, runId = "run", runSequence = 3L),
        )
        restore(rows, messages.take(2), ImportStrategy.REPLACE)
        assertEquals(encoded, dao.getMessage("input")?.sourceJson)
        assertNotNull(dao.getMessage("answer"))
        assertNull(dao.getMessage("answer")?.sourceJson)
        restore(rows, listOf(
            messages[2].copy(parentId = null, runSequence = 0L),
            messages[3].copy(runSequence = 1L),
        ), ImportStrategy.REPLACE)
        assertEquals("Keep text", dao.getMessage("malformed")?.text)
        assertNull(dao.getMessage("malformed")?.sourceJson)
        assertNotNull(dao.getMessage("legacy"))
        assertNull(dao.getMessage("legacy")?.sourceJson)
    }

    @Test
    fun replaceWithLegacyHeaderDoesNotRetainOldTodoOrSubagentCompletion() = runTest {
        dao.upsertConversation(ChatEntity("parent", "Old parent", todoJson = todo))
        dao.upsertConversation(ChatEntity("child", "Old child", parentConversationId = "parent",
            todoJson = todo, subagentResultJson = SubagentCompletion.encode(result),
            subagentResultId = result.id, subagentResultStatus = result.status))
        restore(listOf(NativeExportChatEntity("child", "Legacy root", 1L)), emptyList(), ImportStrategy.REPLACE)
        assertEquals(listOf("child"), dao.getAllConversationIds())
        val restored = checkNotNull(dao.getConversation("child"))
        assertNull(restored.todoJson)
        assertNull(restored.parentConversationId)
        assertNull(restored.subagentResultJson)
        assertNull(restored.subagentResultId)
        assertNull(restored.subagentResultStatus)
        assertNull(restored.subagentDeliveredResultId)
    }

    @Test
    fun invalidTodoAndCompletionHeadersLeaveExistingRowsUnchangedForBothStrategies() = runTest {
        val existing = ChatEntity("existing", "Keep", todoJson = todo)
        dao.upsertConversation(existing)
        val invalid = listOf(
            NativeExportChatEntity("incoming", "Bad Todo", 1L,
                todoJson = "[{\"content\":\"Bad\",\"status\":\"unknown\"}]"),
            NativeExportChatEntity("incoming", "Missing parent", 1L, parentConversationId = "missing"),
            NativeExportChatEntity("incoming", "Mismatched result", 1L,
                subagentResultJson = SubagentCompletion.encode(result), subagentResultId = "wrong",
                subagentResultStatus = result.status),
        )
        for (strategy in listOf(ImportStrategy.MERGE, ImportStrategy.REPLACE)) {
            for (row in invalid) {
                val failure = runCatching { restore(listOf(row), emptyList(), strategy) }.exceptionOrNull()
                assertTrue("Expected rejected ${row.title} for $strategy", failure is IllegalArgumentException)
                assertEquals(listOf(existing.id), dao.getAllConversationIds())
                assertEquals(existing, dao.getConversation(existing.id))
                assertNull(dao.getConversation("incoming"))
            }
        }
    }

    @Test
    fun selectedDeleteRemovesOnlyOwnedFamilyAndClearRemovesRemainingDurableMetadata() = runTest {
        val repository = ConversationRepository(dao, database, scheduleMaintenance = {})
        dao.upsertConversation(ChatEntity("parent", "Parent", todoJson = todo))
        dao.upsertConversation(ChatEntity("child", "Child", parentConversationId = "parent",
            todoJson = todo, subagentResultJson = SubagentCompletion.encode(result),
            subagentResultId = result.id, subagentResultStatus = result.status,
            subagentDeliveredResultId = result.id))
        val unrelated = ChatEntity("unrelated", "Unrelated", todoJson = todo)
        dao.upsertConversation(unrelated)
        repository.deleteConversation("child")
        assertNull(dao.getConversation("child"))
        assertNotNull(dao.getConversation("parent"))
        assertEquals(unrelated, dao.getConversation(unrelated.id))
        dao.upsertConversation(ChatEntity("child", "Child", parentConversationId = "parent",
            todoJson = todo, subagentResultJson = SubagentCompletion.encode(result),
            subagentResultId = result.id, subagentResultStatus = result.status))
        repository.deleteConversation("parent")
        assertNull(dao.getConversation("parent"))
        assertNull(dao.getConversation("child"))
        assertEquals(unrelated, dao.getConversation(unrelated.id))
        dao.deleteAllConversations()
        assertTrue(dao.getAllConversationIds().isEmpty())
        assertNull(dao.getConversation(unrelated.id))
    }

    private suspend fun restore(
        rows: List<NativeExportChatEntity>,
        messages: List<NativeExportMessageEntity>,
        strategy: ImportStrategy,
    ) {
        val index = JsonObject(mapOf("conversations" to JsonArray(rows.map { row ->
            JsonObject(mapOf(
                "id" to JsonPrimitive(row.id), "dataChangedAt" to JsonPrimitive(1L),
                "entry" to JsonPrimitive(NativeBackupFormat.conversationEntry(row.id)),
                "mediaEntries" to JsonArray(emptyList()),
            ))
        })))
        val entries = rows.associate { row ->
            val ownMessages = messages.filter { it.conversationId == row.id }
            val runs = if (ownMessages.isEmpty()) emptyList() else listOf(
                NativeExportRunEntity("run", row.id, startedAt = 1L, lastCheckpointAt = 5L,
                    endedAt = 5L, status = "COMPLETED"),
            )
            NativeBackupFormat.conversationEntry(row.id) to json.encodeToString(
                NativeConversationPayload.serializer(), NativeConversationPayload(
                    listOf(row), runs, ownMessages, emptyList(),
                ),
            )
        } + mapOf(
            NativeBackupFormat.CONVERSATION_INDEX_ENTRY to index.toString(),
            NativeBackupFormat.TASKS_ENTRY to "{\"tasks\":[]}",
        )
        val importer = NativeConversationGraphImporter(database, dao, json,
            NativeConversationMediaRestorer(context, json), scheduleMaintenance = {})
        val media = NativeConversationMediaRestorer.RestoredMedia(emptyMap(), emptyMap(), emptyMap(), emptyList())
        NativeConversationGraphSource.open(Entries(entries), NativeBackupFormat.CURRENT_VERSION,
            temporaryFolder.root).use { graph ->
            val headers = graph.open().use {
                importer.readConversationGraphHeaders(it, strategy, media) { id -> id }
            }
            importer.importConversationGraph(graph, strategy, headers, media, NativeBackupFormat.CURRENT_VERSION,
                semanticModelSnapshot("", emptyList()))
        }
    }

    private class Entries(private val entries: Map<String, String>) : NativeGraphEntrySource {
        override fun has(name: String) = name in entries
        override fun bytes(name: String) = entries[name]?.encodeToByteArray()
        override fun stream(name: String): InputStream? = bytes(name)?.let(::ByteArrayInputStream)
    }
}
