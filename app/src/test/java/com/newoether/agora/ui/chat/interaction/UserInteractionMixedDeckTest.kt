package com.newoether.agora.ui.chat.interaction

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTextInput
import com.newoether.agora.R
import com.newoether.agora.viewmodel.AskUserController
import com.newoether.agora.viewmodel.ShellConfirmationController
import kotlinx.coroutines.CompletableDeferred
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class UserInteractionMixedDeckTest {
    @get:Rule val compose = createComposeRule()

    private var owner by mutableStateOf("c")
    private var questions by mutableStateOf(emptyList<AskUserController.Request>())
    private var shell by mutableStateOf<ShellConfirmationController.PendingShellCommand?>(null)
    private var pages by mutableStateOf(emptyMap<String, String>())
    private var folded by mutableStateOf(emptySet<String>())
    private val drafts = QuestionDrafts()
    private val submitted = mutableListOf<List<Pair<Long, AskUserController.Answer>>>()
    private val skipped = mutableListOf<Long>()
    private val decisions = mutableListOf<Triple<Long, Boolean, Boolean>>()
    private val pageChanges = mutableListOf<Pair<String, String>>()

    @Test
    fun shellIsFirstAndEqualNumericQuestionIdCannotApproveItOnSend() {
        val pending = command(7)
        shell = pending
        questions = listOf(question(7))
        mount()
        compose.onNodeWithText("1 / 2").assertExists()
        assertEquals("shell:7", pages["c"])
        click(R.string.ask_user_next)
        compose.onNodeWithText("Question 7?").assertExists()
        compose.onNode(hasSetTextAction()).performTextInput("answer seven")
        click(R.string.ask_user_send)
        assertEquals(listOf(7L), submitted.single().map { it.first })
        assertEquals("answer seven", submitted.single().single().second.text)
        assertTrue(decisions.isEmpty())
        assertFalse(pending.deferred.isCompleted)
        assertEquals("shell:7", pages["c"])
        click(R.string.shell_confirm_allow)
        assertEquals(listOf(Triple(7L, true, false)), decisions)
    }

    @Test
    fun shellArrivalKeepsTheInitiallyShownQuestionAndItsTypedDraft() {
        val request = question(4)
        questions = listOf(request)
        mount()
        compose.onNode(hasSetTextAction()).performTextInput("unfinished answer")
        compose.runOnIdle { shell = command(4) }
        compose.onNodeWithText("2 / 2").assertExists()
        compose.onNodeWithText("unfinished answer").assertExists()
        assertEquals("question:4", pages["c"])
        click(R.string.back)
        assertEquals("shell:4", pages["c"])
        click(R.string.ask_user_next)
        compose.onNodeWithText("unfinished answer").assertExists()
        assertEquals("unfinished answer", drafts.of(request).typed)
        assertTrue(submitted.isEmpty() && skipped.isEmpty() && decisions.isEmpty())
    }

    @Test
    fun questionArrivalKeepsTheWaitingShellAndItsTrustChoice() {
        shell = command(8)
        mount()
        click(R.string.shell_confirm_always)
        compose.runOnIdle { questions = listOf(question(8)) }
        compose.onNodeWithText("1 / 2").assertExists()
        assertEquals("shell:8", pages["c"])
        click(R.string.ask_user_next)
        compose.onNode(hasSetTextAction()).performTextInput("draft survives approval")
        click(R.string.back)
        click(R.string.shell_confirm_allow)
        assertEquals(listOf(Triple(8L, true, true)), decisions)
        compose.onNodeWithText("draft survives approval").assertExists()
        assertEquals("question:8", pages["c"])
        assertTrue(submitted.isEmpty() && skipped.isEmpty())
    }

    @Test
    fun removedCurrentPageFallsBackAndRemovedShellLeavesQuestionDraftIntact() {
        val first = question(2)
        questions = listOf(first, question(3))
        shell = command(1)
        mount()
        click(R.string.ask_user_next)
        compose.onNode(hasSetTextAction()).performTextInput("keep this")
        click(R.string.ask_user_next)
        assertEquals("question:3", pages["c"])
        compose.runOnIdle { questions = listOf(first) }
        compose.onNodeWithText(text(R.string.shell_confirm_allow)).assertExists()
        assertEquals("shell:1", pages["c"])
        compose.runOnIdle { shell = null }
        compose.onNodeWithText("keep this").assertExists()
        assertEquals("question:2", pages["c"])
        assertTrue(submitted.isEmpty() && skipped.isEmpty() && decisions.isEmpty())
    }

    @Test
    fun foldingAndSkippingQuestionsNeverResolveTheWaitingShell() {
        val pending = command(9)
        shell = pending
        questions = listOf(question(9), question(10))
        mount()
        click(R.string.ask_user_next)
        compose.onNodeWithContentDescription(text(R.string.interaction_minimize)).performClick()
        compose.onNodeWithContentDescription(text(R.string.interaction_expand)).assertExists()
        assertTrue(submitted.isEmpty() && skipped.isEmpty() && decisions.isEmpty())
        compose.onNodeWithContentDescription(text(R.string.interaction_expand)).performClick()
        compose.onNodeWithText("2 / 3").assertExists()
        click(R.string.ask_user_skip)
        assertEquals(listOf(9L, 10L), skipped)
        assertTrue(decisions.isEmpty())
        assertFalse(pending.deferred.isCompleted)
        compose.onNodeWithText(text(R.string.shell_confirm_allow)).assertExists()
        click(R.string.shell_confirm_deny)
        assertEquals(listOf(Triple(9L, false, false)), decisions)
    }

    @Test
    fun outgoingShellIsHiddenAndItsSemanticDecisionCannotMutateTheNextOwner() {
        shell = command(11)
        questions = listOf(question(12, "d"))
        mount()
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { owner = "d" }
        Snapshot.sendApplyNotifications()
        compose.mainClock.advanceTimeByFrame()
        compose.waitForIdle()
        compose.onNodeWithText(text(R.string.shell_confirm_allow)).assertDoesNotExist()
        compose.onNodeWithText(text(R.string.shell_confirm_allow), useUnmergedTree = true).performClick()
        assertTrue(decisions.isEmpty())
        assertTrue(submitted.isEmpty() && skipped.isEmpty())
        compose.mainClock.advanceTimeBy(1_000)
        compose.waitForIdle()
        compose.onNodeWithText("Question 12?").assertExists()
        assertEquals("question:12", pages["d"])
        assertEquals("shell:11", pages["c"])
    }

    @Test
    fun outgoingQuestionCannotSubmitSkipFoldOrChangePageForTheNextOwner() {
        val old = question(13)
        questions = listOf(old, question(14, "d"))
        drafts.of(old).typed = "old answer"
        mount()
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { owner = "d" }
        Snapshot.sendApplyNotifications()
        compose.mainClock.advanceTimeByFrame()
        compose.waitForIdle()
        val previousPages = pageChanges.toList()
        compose.onNodeWithText(text(R.string.ask_user_send)).assertDoesNotExist()
        compose.onNodeWithText(text(R.string.ask_user_send), useUnmergedTree = true).performClick()
        compose.onNodeWithText(text(R.string.ask_user_skip), useUnmergedTree = true).performClick()
        compose.onNodeWithContentDescription(text(R.string.interaction_minimize), useUnmergedTree = true)
            .performClick()
        assertTrue(submitted.isEmpty() && skipped.isEmpty() && decisions.isEmpty())
        assertTrue(folded.isEmpty())
        assertEquals(previousPages, pageChanges)
        assertEquals("old answer", drafts.of(old).typed)
        compose.mainClock.advanceTimeBy(1_000)
        compose.waitForIdle()
        compose.onNodeWithText("Question 14?").assertExists()
        assertEquals("question:14", pages["d"])
    }

    private fun mount() {
        compose.setContent {
            MaterialTheme {
                UserInteractionBar(
                    conversationId = owner,
                    interactions = userInteractions(owner, questions, shell),
                    autoWrapCodeBlocks = false,
                    onSubmitQuestions = { answers ->
                        submitted += answers
                        questions = questions.filterNot { request -> answers.any { it.first == request.id } }
                    },
                    onSkipQuestion = { id ->
                        skipped += id
                        questions = questions.filterNot { it.id == id }
                    },
                    onShellDecision = { id, allow, always ->
                        decisions += Triple(id, allow, always)
                        if (shell?.id == id) shell = null
                    },
                    onHeightChanged = {},
                    minimizedIn = folded,
                    onMinimizedChange = { id, minimized ->
                        folded = if (minimized) folded + id else folded - id
                    },
                    drafts = drafts,
                    pageIn = pages,
                    onPageChange = { id, key ->
                        pageChanges += id to key
                        pages = pages + (id to key)
                    },
                    onCardGone = {},
                )
            }
        }
        compose.waitForIdle()
    }

    private fun click(id: Int) {
        compose.onNodeWithText(text(id)).performClick()
        compose.waitForIdle()
    }

    private fun text(id: Int) = RuntimeEnvironment.getApplication().getString(id)

    private fun question(id: Long, conversation: String = "c") = AskUserController.Request(
        id = id, conversationId = conversation, question = "Question $id?", options = emptyList(),
        allowMultiple = false, blocking = false,
    )

    private fun command(id: Long) = ShellConfirmationController.PendingShellCommand(
        id = id, conversationId = "c", server = "test-server", summary = "echo test-$id",
        deferred = CompletableDeferred(),
    )
}
