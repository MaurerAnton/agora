package com.newoether.agora.tool

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.EmbeddingEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.model.CitationPolicy
import com.newoether.agora.model.Participant
import com.newoether.agora.model.toMessageSegment
import com.newoether.agora.model.RunStatus
import com.newoether.agora.viewmodel.GenerationContext
import java.nio.ByteBuffer
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class SubagentHistoryIsolationTest {
    private lateinit var database: ChatDatabase
    private lateinit var repository: ConversationRepository
    private lateinit var provider: RagToolProvider
    private val context = GenerationContext(conversationId = "parent", accessPastConversations = true)

    @Before
    fun setUp() = runTest {
        database = Room.inMemoryDatabaseBuilder(
            ApplicationProvider.getApplicationContext<Context>(), ChatDatabase::class.java,
        ).allowMainThreadQueries().build()
        repository = ConversationRepository(database.chatDao(), database)
        provider = RagToolProvider(repository)
        insert("parent", 1L)
        insert("other", 2L)
        insert("owned-child", 100L, parentId = "parent")
        insert("foreign-child", 101L, parentId = "other")
        insert("sibling", 102L, parentId = "parent")
        insert("task", 103L, taskId = "scheduled")
    }

    @After
    fun tearDown() = database.close()

    @Test
    fun historyCountAndBothPageOrdersExcludeChildrenBeforePagination() = runTest {
        for ((order, expected) in listOf("asc" to listOf("parent", "other"), "desc" to listOf("other", "parent"))) {
            for ((offset, id) in expected.withIndex()) {
                val result = execute("list_conversations", """{"order":"$order","limit":1,"offset":$offset}""")
                assertEquals(2, result.getValue("total").jsonPrimitive.content.toInt())
                assertEquals((offset == 0).toString(), result.getValue("has_more").jsonPrimitive.content)
                assertEquals(id, result.getValue("conversations").jsonArray.single().jsonObject
                    .getValue("id").jsonPrimitive.content)
            }
        }
        val exhausted = execute("list_conversations", """{"offset":2}""")
        assertTrue(exhausted.getValue("conversations").jsonArray.isEmpty())
    }

    @Test
    fun keywordAndCitationMatchesCannotExposeChildrenOrDisplaceTheRequestedRoot() = runTest {
        val dao = database.chatDao()
        assertEquals(listOf("other-message"), repository.searchMessages("history body", 1).map { it.id })
        assertEquals(listOf("other-message", "parent-message"),
            repository.searchMessages("source needle", 20).map { it.id })
        assertEquals(setOf("parent-message", "other-message"),
            dao.getMessagesWithCitationSegmentsPage("", 20).map { it.id }.toSet())
        val result = execute("search_conversations", """{"query":"history body","limit":1}""")
        assertEquals("other", result.getValue("results").jsonArray.single().jsonObject
            .getValue("conversation_id").jsonPrimitive.content)
        assertFalse(result.toString().contains("owned-child"))
        assertFalse(result.toString().contains("foreign-child"))
    }

    @Test
    fun semanticPagesAndFinalHydrationExcludeChildrenWithoutRemovingTheirStoredData() = runTest {
        val dao = database.chatDao()
        val owners = listOf("owned-child", "foreign-child", "sibling", "task", "other", "parent")
        for (owner in owners) {
            dao.insertEmbeddings(listOf(EmbeddingEntity(
                messageId = "$owner-message", modelId = "embedding",
                embedding = ByteBuffer.allocate(4).putFloat(1f).array(),
                chunkText = "history body $owner", dimension = 1,
            )))
        }
        val first = dao.getEmbeddingSearchPage("embedding", 0L, 10, 1).single()
        assertEquals("other-message", first.messageId)
        val second = dao.getEmbeddingSearchPage("embedding", first.id, 10, 1).single()
        assertEquals("parent-message", second.messageId)
        assertTrue(dao.getEmbeddingSearchPage("embedding", second.id, 10, 1).isEmpty())
        assertEquals(setOf("parent-message", "other-message"),
            dao.getSearchableMessagesByIds(owners.map { "$it-message" }).map { it.id }.toSet())
        assertTrue(dao.getMessage("owned-child-message") != null)
        assertTrue(dao.getEmbedding("owned-child-message") != null)
    }

    @Test
    fun onlyTheDirectParentCanReadAChildEvenWhenAnotherCallerKnowsItsId() = runTest {
        val allowed = execute("read_conversation", """{"conversation_id":"owned-child"}""")
        assertEquals("history body owned-child", allowed.getValue("messages").jsonArray.single()
            .jsonObject.getValue("text").jsonPrimitive.content)
        for (caller in listOf(null, "other", "owned-child", "sibling")) {
            val rejected = execute("read_conversation", """{"conversation_id":"owned-child"}""",
                context.copy(conversationId = caller))
            assertEquals("not_found", rejected.getValue("error").jsonPrimitive.content)
            assertFalse(rejected.containsKey("title"))
            assertFalse(rejected.containsKey("messages"))
        }
        for (id in listOf("foreign-child", "task", "missing")) {
            assertEquals("not_found", execute("read_conversation", """{"conversation_id":"$id"}""")
                .getValue("error").jsonPrimitive.content)
        }
        assertNull(repository.getSearchableConversation("owned-child"))
        val ordinary = execute("read_conversation", """{"conversation_id":"other"}""")
        assertEquals("other", ordinary.getValue("conversation_id").jsonPrimitive.content)
        assertTrue(ordinary.containsKey("messages"))
    }

    private suspend fun execute(name: String, args: String, caller: GenerationContext = context) =
        Json.parseToJsonElement(provider.execute(name, args, caller)).jsonObject

    private suspend fun insert(id: String, timestamp: Long, parentId: String? = null, taskId: String? = null) {
        val dao = database.chatDao()
        dao.upsertConversation(ChatEntity(id = id, title = id, lastUpdated = timestamp,
            parentConversationId = parentId, taskId = taskId))
        dao.upsertRun(RunEntity(id = "$id-run", conversationId = id, parentRunId = null,
            status = RunStatus.ACTIVE, activeSlot = 1, startedAt = timestamp, lastCheckpointAt = timestamp))
        dao.insertMessage(MessageEntity(id = "$id-message", conversationId = id,
            text = "history body $id", participant = Participant.MODEL, timestamp = timestamp,
            runId = "$id-run", runSequence = 0,
            toolCallJson = Json.encodeToString(listOf(requireNotNull(CitationPolicy.create(
                provider = "test", kind = "web", title = "source needle", url = "https://example.com",
            )).toMessageSegment()))))
    }
}
