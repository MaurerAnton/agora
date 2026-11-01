package com.newoether.agora.webui

import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.model.SubagentSummary
import io.mockk.every
import io.mockk.mockk
import io.mockk.verify
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class WebUiSubagentsTest {
    private val dao = mockk<ChatDao>()
    private val repository = ConversationRepository(dao, database = null, scheduleMaintenance = {})

    @Test
    fun emptySelectionClearsCapsulesWithoutReadingAnyOwner() = runTest {
        val events = Channel<WebSyncEvent>(Channel.UNLIMITED)
        repository.forwardSubagents(null, 7, events)
        assertEquals(WebSyncEvent.Subagents(null, 7, null, emptyList()), events.receive())
        verify(exactly = 0) { dao.observeParentConversationId(any()) }
        verify(exactly = 0) { dao.observeSubagents(any()) }
    }

    @Test
    fun rootSelectionObservesOnlyItsNarrowProjectionWithExactSequence() = runTest {
        val children = MutableStateFlow(listOf(child("first", "parent", "running")))
        every { dao.observeParentConversationId("parent") } returns flowOf(null)
        every { dao.observeSubagents("parent") } returns children
        val events = Channel<WebSyncEvent>(Channel.UNLIMITED)
        val job = backgroundScope.launch { repository.forwardSubagents("parent", 11, events) }
        runCurrent()
        assertEquals(WebSyncEvent.Subagents("parent", 11, null, children.value), events.receive())
        children.value = listOf(child("first", "parent", "completed"), child("second", "parent", "running"))
        runCurrent()
        assertEquals(WebSyncEvent.Subagents("parent", 11, null, children.value), events.receive())
        verify(exactly = 1) { dao.observeParentConversationId("parent") }
        verify(exactly = 1) { dao.observeSubagents("parent") }
        verify(exactly = 0) { dao.getAllConversations() }
        job.cancelAndJoin()
        children.value = emptyList()
        runCurrent()
        assertFalse(events.tryReceive().isSuccess)
    }

    @Test
    fun childSelectionShowsSiblingsAndCancelsTheReplacedParentObservation() = runTest {
        val parent = MutableStateFlow<String?>("parent")
        val siblings = MutableStateFlow(listOf(child("selected", "parent", "running")))
        val reparented = MutableStateFlow(listOf(child("selected", "other", "idle")))
        every { dao.observeParentConversationId("selected") } returns parent
        every { dao.observeSubagents("parent") } returns siblings
        every { dao.observeSubagents("other") } returns reparented
        val events = Channel<WebSyncEvent>(Channel.UNLIMITED)
        val job = backgroundScope.launch { repository.forwardSubagents("selected", 23, events) }
        runCurrent()
        assertEquals(WebSyncEvent.Subagents("selected", 23, "parent", siblings.value), events.receive())
        parent.value = "other"
        runCurrent()
        assertEquals(WebSyncEvent.Subagents("selected", 23, "other", reparented.value), events.receive())
        siblings.value = emptyList()
        runCurrent()
        assertFalse(events.tryReceive().isSuccess)
        verify(exactly = 0) { dao.observeSubagents("selected") }
        job.cancelAndJoin()
        reparented.value = emptyList()
        runCurrent()
        assertFalse(events.tryReceive().isSuccess)
    }

    private fun child(id: String, parent: String, status: String) =
        SubagentSummary(id, "Agent $id", "provider:model", parent, status)
}
