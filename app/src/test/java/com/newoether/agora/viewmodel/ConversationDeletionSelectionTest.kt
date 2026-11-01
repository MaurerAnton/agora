package com.newoether.agora.viewmodel

import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.util.DebugLog
import io.mockk.Runs
import io.mockk.just
import io.mockk.mockkObject
import io.mockk.unmockkObject
import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.After
import org.junit.Before
import org.junit.Test

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
class ConversationDeletionSelectionTest {
    @Before
    fun stubAndroidLog() {
        mockkObject(DebugLog)
        every { DebugLog.e(any(), any()) } just Runs
    }

    @After
    fun restoreAndroidLog() = unmockkObject(DebugLog)

    @Test
    fun selectedChildReturnsToItsSurvivingParentThroughOrdinarySelection() = runTest {
        val fixture = Fixture(backgroundScope)
        coEvery { fixture.conversations.getConversation("parent") } returns
            ChatEntity("parent", "Parent", modelId = "parent-model")
        fixture.controller.publishAcceptedConversation("child", "child-model")

        fixture.controller.settleDeletedSelectedConversation("child", "parent")
        runCurrent()

        assertEquals("parent", fixture.controller.currentConversationId.value)
        assertEquals("parent-model", fixture.controller.currentActiveModel.value)
        assertFalse(fixture.controller.isNewChatMode.value)
        assertEquals(0, fixture.clears)
        val request = requireNotNull(fixture.controller.switchingScrollRequest.value)
        assertTrue(request.readyForUi)
        assertFalse(request.hapticOnCompletion)
        assertTrue(fixture.controller.completeSwitchingScroll(request.id))
    }

    @Test
    fun parentDisappearingBeforeSelectionFallsBackToNewChat() = runTest {
        val fixture = Fixture(backgroundScope)
        coEvery { fixture.conversations.getConversation("parent") } returns null
        fixture.controller.publishAcceptedConversation("child", "child-model")

        fixture.controller.settleDeletedSelectedConversation("child", "parent")
        runCurrent()

        assertNull(fixture.controller.currentConversationId.value)
        assertTrue(fixture.controller.isNewChatMode.value)
        assertEquals(1, fixture.clears)
        assertFalse(fixture.controller.isSwitching.value)
    }

    @Test
    fun newerPendingSelectionWinsBeforeParentReturnIsRequested() = runTest {
        val fade = CompletableDeferred<Unit>()
        val fixture = Fixture(backgroundScope) { fade.await() }
        coEvery { fixture.conversations.getConversation("other") } returns ChatEntity("other", "Other")
        fixture.controller.publishAcceptedConversation("child", "child-model")
        fixture.controller.selectConversation("other")

        fixture.controller.settleDeletedSelectedConversation("child", "parent")
        fade.complete(Unit)
        runCurrent()

        assertEquals("other", fixture.controller.currentConversationId.value)
        assertEquals(0, fixture.clears)
        coVerify(exactly = 0) { fixture.conversations.getConversation("parent") }
    }

    @Test
    fun delayedDeleteCoverCannotReplaceANewerPendingSelection() = runTest {
        val fade = CompletableDeferred<Unit>()
        var fades = 0
        val fixture = Fixture(backgroundScope) { if (fades++ == 0) fade.await() }
        coEvery { fixture.conversations.getConversation("other") } returns ChatEntity("other", "Other")
        fixture.controller.publishAcceptedConversation("child", "child-model")
        fixture.controller.selectConversation("other")
        runCurrent()
        val navigation = requireNotNull(fixture.controller.switchingScrollRequest.value)

        // The deletion coroutine starts after navigation, while the old owner is still published.
        assertNull(fixture.controller.beginTreeMutation("child", scrollToTarget = false))
        assertEquals(navigation, fixture.controller.switchingScrollRequest.value)
        assertEquals("child", fixture.controller.currentConversationId.value)
        fixture.controller.settleDeletedSelectedConversation("child", "parent")
        fixture.controller.failTreeMutation(null)
        assertEquals(navigation, fixture.controller.switchingScrollRequest.value)

        fade.complete(Unit)
        runCurrent()

        assertEquals("other", fixture.controller.currentConversationId.value)
        assertEquals(navigation.id, fixture.controller.switchingScrollRequest.value?.id)
        assertTrue(fixture.controller.switchingScrollRequest.value?.readyForUi == true)
        assertEquals(0, fixture.clears)
        coVerify(exactly = 0) { fixture.conversations.getConversation("parent") }
    }

    @Test
    fun navigationDuringParentLoadCannotBeReplacedByTheFallback() = runTest {
        val load = CompletableDeferred<Unit>()
        val fixture = Fixture(backgroundScope)
        coEvery { fixture.conversations.getConversation("parent") } coAnswers {
            load.await()
            null
        }
        coEvery { fixture.conversations.getConversation("other") } returns ChatEntity("other", "Other")
        fixture.controller.publishAcceptedConversation("child", "child-model")
        fixture.controller.settleDeletedSelectedConversation("child", "parent")
        runCurrent()

        fixture.controller.selectConversation("other")
        load.complete(Unit)
        runCurrent()

        assertEquals("other", fixture.controller.currentConversationId.value)
        assertFalse(fixture.controller.isNewChatMode.value)
        assertEquals(0, fixture.clears)
    }

    @Test
    fun explicitNewChatDuringParentReturnRemainsAuthoritative() = runTest {
        val fade = CompletableDeferred<Unit>()
        val fixture = Fixture(backgroundScope) { fade.await() }
        fixture.controller.publishAcceptedConversation("child", "child-model")
        fixture.controller.settleDeletedSelectedConversation("child", "parent")

        fixture.controller.createNewChat()
        fade.complete(Unit)
        runCurrent()

        assertNull(fixture.controller.currentConversationId.value)
        assertTrue(fixture.controller.isNewChatMode.value)
        assertEquals(1, fixture.clears)
        coVerify(exactly = 0) { fixture.conversations.getConversation("parent") }
    }

    private class Fixture(scope: CoroutineScope, fadeDelay: suspend () -> Unit = {}) {
        val conversations = mockk<ConversationRepository>()
        private val store = ConversationRenderStore()
        private val workspaces = mockk<ConversationWorkspaceStore>(relaxed = true) {
            every { newChatModelId } returns MutableStateFlow(null)
        }
        var clears = 0
        val controller = ConversationSelectionController(
            scope = scope,
            conversations = conversations,
            registry = ConversationStateRegistry {},
            defaultModel = MutableStateFlow("parent-model"),
            validModels = MutableStateFlow(setOf("parent-model", "child-model")),
            scrollRequests = ScrollRequestCoordinator(),
            renderStore = { store },
            clearConversationGraph = { clears += 1 },
            workspaces = workspaces,
            abortRegeneration = {},
            fadeDelay = fadeDelay,
        )
    }
}
