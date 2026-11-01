package com.newoether.agora.viewmodel

import com.newoether.agora.model.ConversationTodoItem
import com.newoether.agora.model.SubagentSummary
import com.newoether.agora.ui.chat.interaction.UserInteraction
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ConversationContentReadinessTest {
    @Test
    fun delayedTodoAndFamilyRequireRealInitialSnapshotsInEitherOrder() = runTest {
        for (todoFirst in listOf(true, false)) for (todoEmpty in listOf(true, false))
            for (agentsEmpty in listOf(true, false)) {
            val todos = if (todoEmpty) emptyList() else listOf(ConversationTodoItem("Task"))
            val children = if (agentsEmpty) emptyList() else listOf(agent("child", "root"))
            val selected = MutableStateFlow<String?>("root")
            val loaded = MutableStateFlow<String?>("root")
            val todoRows = MutableSharedFlow<List<ConversationTodoItem>>()
            val familyRows = MutableSharedFlow<List<SubagentSummary>>()
            val todo = observeSelectedConversationTodo(selected) { todoRows }
                .stateIn(backgroundScope, SharingStarted.Eagerly, null to emptyList())
            val family = observeSelectedSubagentFamily(selected, { MutableStateFlow(null) }, { familyRows })
                .stateIn(backgroundScope, SharingStarted.Eagerly, SubagentFamilySnapshot())
            val requests = MutableStateFlow("root" to emptyList<UserInteraction>())
            val ready = observeConversationContentReadiness(selected, loaded, todo, family, requests)
                .stateIn(backgroundScope, SharingStarted.Eagerly, null)
            runCurrent()
            assertNull(todo.value.first)
            assertNull(ready.value)

            if (todoFirst) todoRows.emit(todos) else familyRows.emit(children)
            runCurrent()
            assertNull(ready.value)
            if (todoFirst) familyRows.emit(children) else todoRows.emit(todos)
            runCurrent()
            assertEquals("root", ready.value)
            assertEquals("root", todo.value.first)
            assertEquals(todos, todo.value.second)
            assertEquals(children, family.value.agents)

            // A later live mutation updates the existing projection without reopening the cover.
            familyRows.emit(listOf(agent("child", "root")))
            runCurrent()
            assertEquals("root", ready.value)
            assertEquals(listOf("child"), family.value.agents.map { it.id })
        }
    }

    @Test
    fun initialInteractionsAndStableMessagesAreIndependentRequiredInputs() = runTest {
        val selected = MutableStateFlow<String?>("root")
        val loaded = MutableStateFlow<String?>(null)
        val todo = MutableStateFlow<Pair<String?, List<ConversationTodoItem>>>("root" to emptyList())
        val family = MutableStateFlow(SubagentFamilySnapshot("root"))
        val requests = MutableSharedFlow<Pair<String?, List<UserInteraction>>>()
        val ready = observeConversationContentReadiness(selected, loaded, todo, family, requests)
            .stateIn(backgroundScope, SharingStarted.Eagerly, null)
        runCurrent()
        requests.emit("other" to emptyList())
        loaded.value = "root"
        runCurrent()
        assertNull(ready.value)
        requests.emit("root" to emptyList())
        runCurrent()
        assertEquals("root", ready.value)
        loaded.value = null
        runCurrent()
        assertNull(ready.value)
    }

    @Test
    fun cancelledOwnerAndReturnToSameOwnerCannotReuseUnobservedTodo() = runTest {
        val selected = MutableStateFlow<String?>("root")
        val loaded = MutableStateFlow<String?>("root")
        val rows = mutableMapOf<String, MutableSharedFlow<List<ConversationTodoItem>>>()
        val started = mutableListOf<String>()
        val todo = observeSelectedConversationTodo(selected) { id ->
            started += id
            rows.getOrPut(id) { MutableSharedFlow() }
        }.stateIn(backgroundScope, SharingStarted.Eagerly, null to emptyList())
        val family = MutableStateFlow(SubagentFamilySnapshot("root"))
        val requests = selected.combine(MutableStateFlow(emptyList<UserInteraction>())) { id, values -> id to values }
        val ready = observeConversationContentReadiness(selected, loaded, todo, family, requests)
            .stateIn(backgroundScope, SharingStarted.Eagerly, null)
        runCurrent()
        rows.getValue("root").emit(emptyList())
        runCurrent()
        assertEquals("root", ready.value)

        selected.value = "other"
        loaded.value = "other"
        runCurrent()
        family.value = SubagentFamilySnapshot("other")
        rows.getValue("root").emit(emptyList())
        runCurrent()
        assertNull(todo.value.first)
        assertNull(ready.value)

        selected.value = "root"
        loaded.value = "root"
        family.value = SubagentFamilySnapshot("root")
        runCurrent()
        assertNull(ready.value)
        rows.getValue("other").emit(emptyList())
        runCurrent()
        assertNull(ready.value)
        rows.getValue("root").emit(emptyList())
        runCurrent()
        assertEquals("root", ready.value)
        assertEquals(listOf("root", "other", "root"), started)
        selected.value = null
        loaded.value = null
        runCurrent()
        assertNull(ready.value)
        assertNull(todo.value.first)
    }

    @Test
    fun sameFamilyNavigationRetainsExactAgentsWhileWaitingForSelectedTodo() = runTest {
        val selected = MutableStateFlow<String?>("root")
        val loaded = MutableStateFlow<String?>("root")
        val rows = mutableMapOf<String, MutableSharedFlow<List<ConversationTodoItem>>>()
        val familyStarts = mutableListOf<String>()
        val agents = listOf(agent("child", "root"))
        val family = observeSelectedSubagentFamily(
            selected,
            { id -> MutableStateFlow(if (id == "root") null else "root") },
            { id -> familyStarts += id; MutableStateFlow(agents) },
        ).stateIn(backgroundScope, SharingStarted.Eagerly, SubagentFamilySnapshot())
        val todo = observeSelectedConversationTodo(selected) { id ->
            rows.getOrPut(id) { MutableSharedFlow() }
        }.stateIn(backgroundScope, SharingStarted.Eagerly, null to emptyList())
        val requests = selected.combine(MutableStateFlow(emptyList<UserInteraction>())) { id, values -> id to values }
        val ready = observeConversationContentReadiness(selected, loaded, todo, family, requests)
            .stateIn(backgroundScope, SharingStarted.Eagerly, null)
        runCurrent()
        rows.getValue("root").emit(emptyList())
        runCurrent()
        val retained = family.value
        selected.value = "child"
        loaded.value = "child"
        runCurrent()
        assertSame(retained, family.value.forSelection("child"))
        assertSame(agents, family.value.agents)
        assertNull(ready.value)
        rows.getValue("child").emit(emptyList())
        runCurrent()
        assertEquals("child", ready.value)
        selected.value = "root"
        loaded.value = "root"
        runCurrent()
        assertSame(retained, family.value)
        assertNull(ready.value)
        rows.getValue("root").emit(emptyList())
        runCurrent()
        assertEquals("root", ready.value)
        assertEquals(listOf("root"), familyStarts)
    }

    @Test
    fun returningToUnrelatedFamilyWaitsForItsNewFirstSnapshot() = runTest {
        val selected = MutableStateFlow<String?>("root")
        val families = mutableMapOf<String, MutableSharedFlow<List<SubagentSummary>>>()
        val family = observeSelectedSubagentFamily(
            selected,
            { MutableStateFlow(null) },
            { id -> families.getOrPut(id) { MutableSharedFlow() } },
        ).stateIn(backgroundScope, SharingStarted.Eagerly, SubagentFamilySnapshot())
        runCurrent()
        families.getValue("root").emit(listOf(agent("child", "root")))
        runCurrent()
        assertEquals("root", family.value.familyId)
        selected.value = "other"
        runCurrent()
        assertNull(family.value.forSelection("other"))
        selected.value = "root"
        runCurrent()
        assertNull(family.value.forSelection("root"))
        families.getValue("other").emit(emptyList())
        runCurrent()
        assertNull(family.value.forSelection("root"))
        families.getValue("root").emit(emptyList())
        runCurrent()
        assertEquals(SubagentFamilySnapshot("root"), family.value)
    }

    private fun agent(id: String, parent: String) = SubagentSummary(id, id, null, parent, "running")
}
