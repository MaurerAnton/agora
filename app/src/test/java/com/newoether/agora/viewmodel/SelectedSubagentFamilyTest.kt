package com.newoether.agora.viewmodel

import com.newoether.agora.model.SubagentSummary
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.emitAll
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class SelectedSubagentFamilyTest {
    private val first = agent("first", "root")
    private val second = agent("second", "root")

    @Test
    fun parentChildAndSiblingSelectionsRetainOneFamilyObserver() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.parent("first", "root")
        fixture.parent("second", "root")
        fixture.family("root").value = listOf(first, second)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()
        val snapshot = fixture.latest
        val emissionCount = fixture.events.size

        listOf("first", "second", "root").forEach { id ->
            fixture.selected.value = id
            runCurrent()
            assertSame(snapshot, fixture.latest.forSelection(id))
            assertEquals(emissionCount, fixture.events.size)
        }
        assertEquals(listOf("root"), fixture.familyStarts)
        assertTrue(fixture.familyStops.isEmpty())
        assertTrue(fixture.events.filter { it.familyId != null }.all { it.agents == listOf(first, second) })
    }

    @Test
    fun knownChildRetainsCapsulesWhileItsParentLookupIsPending() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.family("root").value = listOf(first, second)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()
        val snapshot = fixture.latest

        fixture.selected.value = "second"
        runCurrent()
        assertSame(snapshot, fixture.latest.forSelection("second"))
        assertEquals(listOf("root"), fixture.familyStarts)
        fixture.parent("second", "root")
        runCurrent()
        assertSame(snapshot, fixture.latest)
    }

    @Test
    fun unrelatedSelectionCannotUseRetainedFamilyWhileLookupIsPending() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.family("root").value = listOf(first)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()
        val outgoing = fixture.latest

        fixture.selected.value = "other"
        runCurrent()
        assertSame(outgoing, fixture.latest)
        assertNull(fixture.latest.forSelection(fixture.selected.value))
        fixture.parent("other", null)
        fixture.family("other").value = listOf(agent("other-child", "other"))
        runCurrent()
        assertEquals("other", fixture.latest.familyId)
        assertNull(fixture.latest.forSelection("root"))
        assertEquals(listOf("root", "other"), fixture.familyStarts)
        assertEquals(listOf("root"), fixture.familyStops)
    }

    @Test
    fun newChatClearsFamilyAndCancelsItsObserver() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.family("root").value = listOf(first)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()
        val outgoing = fixture.latest

        fixture.selected.value = null
        assertNull(outgoing.forSelection(null))
        runCurrent()
        assertEquals(SubagentFamilySnapshot(), fixture.latest)
        assertEquals(listOf("root"), fixture.familyStops)
        fixture.family("root").value = listOf(first.copy(status = "completed"))
        runCurrent()
        assertEquals(SubagentFamilySnapshot(), fixture.latest)
    }

    @Test
    fun lateLookupForSupersededSelectionCannotPublishAnotherFamily() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.family("root").value = listOf(first)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()
        fixture.selected.value = "abandoned"
        runCurrent()
        fixture.selected.value = "latest"
        runCurrent()

        fixture.parent("abandoned", "abandoned-parent")
        fixture.parent("latest", "latest-parent")
        fixture.family("latest-parent").value = listOf(agent("latest", "latest-parent"))
        runCurrent()
        assertEquals("latest-parent", fixture.latest.familyId)
        assertFalse(fixture.familyStarts.contains("abandoned-parent"))
        assertNull(fixture.latest.forSelection("abandoned"))
    }

    @Test
    fun statusNameAndMembershipUpdatesKeepFamilyIdentityAndSelectionGate() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.family("root").value = listOf(first, second)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()

        val updated = first.copy(title = "Renamed", status = "running")
        fixture.family("root").value = listOf(updated)
        runCurrent()
        assertEquals("root", fixture.latest.familyId)
        assertEquals(listOf(updated), fixture.latest.forSelection("first")?.agents)
        assertNull(fixture.latest.forSelection("second"))
        assertEquals(listOf("root"), fixture.familyStarts)
    }

    @Test
    fun realEmptyFamilyUpdateIsNotSuppressedBySelectionRetention() = runTest {
        val fixture = Fixture()
        fixture.parent("root", null)
        fixture.family("root").value = listOf(first)
        fixture.collect(backgroundScope)
        fixture.selected.value = "root"
        runCurrent()

        fixture.family("root").value = emptyList()
        runCurrent()
        assertEquals(SubagentFamilySnapshot("root"), fixture.latest)
        assertSame(fixture.latest, fixture.latest.forSelection("root"))
        assertNull(fixture.latest.forSelection("first"))
        assertEquals(listOf("root"), fixture.familyStarts)
    }

    @Test
    fun emptySelectionDoesNotQueryParentsOrFamilies() = runTest {
        val fixture = Fixture()
        fixture.collect(backgroundScope)
        runCurrent()
        assertEquals(SubagentFamilySnapshot(), fixture.latest)
        assertTrue(fixture.parentRequests.isEmpty())
        assertTrue(fixture.familyStarts.isEmpty())
    }

    @Test
    fun retentionRequiresExactParentOrMatchingChildOwnership() {
        val snapshot = SubagentFamilySnapshot("root", listOf(first, agent("foreign", "other")))
        assertSame(snapshot, snapshot.forSelection("root"))
        assertSame(snapshot, snapshot.forSelection("first"))
        assertNull(snapshot.forSelection("foreign"))
        assertNull(snapshot.forSelection("unknown"))
        assertNull(snapshot.forSelection(null))
        assertNull(SubagentFamilySnapshot().forSelection("root"))
    }

    private class Fixture {
        val selected = MutableStateFlow<String?>(null)
        val events = mutableListOf<SubagentFamilySnapshot>()
        val parentRequests = mutableListOf<String>()
        val familyStarts = mutableListOf<String>()
        val familyStops = mutableListOf<String>()
        private val parents = mutableMapOf<String, MutableSharedFlow<String?>>()
        private val families = mutableMapOf<String, MutableStateFlow<List<SubagentSummary>>>()
        val latest get() = events.last()

        fun parent(id: String, parent: String?) {
            check(parentFlow(id).tryEmit(parent))
        }

        private fun parentFlow(id: String) = parents.getOrPut(id) { MutableSharedFlow(replay = 1) }
        fun family(id: String) = families.getOrPut(id) { MutableStateFlow(emptyList()) }

        fun collect(scope: CoroutineScope) = scope.launch {
            observeSelectedSubagentFamily(
                selectedConversationId = selected,
                observeParent = { id ->
                    flow {
                        parentRequests += id
                        emitAll(parentFlow(id))
                    }
                },
                observeSubagents = { id ->
                    flow {
                        familyStarts += id
                        try {
                            emitAll(family(id))
                        } finally {
                            familyStops += id
                        }
                    }
                },
            ).collect { events += it }
        }
    }

    private companion object {
        fun agent(id: String, parent: String) = SubagentSummary(id, id, null, parent, "idle")
    }
}
