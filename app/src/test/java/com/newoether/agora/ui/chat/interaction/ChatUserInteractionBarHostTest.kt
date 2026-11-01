package com.newoether.agora.ui.chat.interaction

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.getUnclippedBoundsInRoot
import androidx.compose.ui.test.hasScrollAction
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.assertCountEquals
import androidx.compose.ui.test.performScrollTo
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.geometry.Offset
import com.newoether.agora.data.SubagentCapsuleDisplayModes
import com.newoether.agora.model.ConversationTodoItem
import com.newoether.agora.model.SubagentSummary
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.unit.dp
import com.newoether.agora.R
import com.newoether.agora.viewmodel.AskUserController
import com.newoether.agora.viewmodel.ChatViewModel
import com.newoether.agora.viewmodel.ShellConfirmationController
import io.mockk.every
import io.mockk.mockk
import io.mockk.verify
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class ChatUserInteractionBarHostTest {
    @get:Rule val compose = createComposeRule()

    private val questions = MutableStateFlow(emptyList<AskUserController.Request>())
    private val shell = MutableStateFlow<ShellConfirmationController.PendingShellCommand?>(null)
    private val askUser = mockk<AskUserController>(relaxed = true)
    private val confirmations = mockk<ShellConfirmationController>(relaxed = true)
    private val viewModel = mockk<ChatViewModel>(relaxed = true)
    private var owner by mutableStateOf("c")
    private var reveal: Any? = null
    private var waiting = false
    private val events = mutableListOf<Any?>()

    @Test
    fun everyNewQuestionInAnExistingSetRevealsButEditsRemovalsAndOtherOwnersDoNot() {
        mount()
        assertNull(reveal)
        assertEquals(listOf<Any?>(null), events)
        val first = question(1, setId = 20)
        update { questions.value = listOf(first) }
        val firstReveal = reveal
        assertNotNull(firstReveal)
        val count = events.size
        update { questions.value = listOf(first.copy(question = "Updated wording")) }
        assertEquals(firstReveal, reveal)
        update { questions.value = listOf(first, question(90, conversation = "other")) }
        assertEquals(count, events.size)
        update { questions.value = listOf(first, question(2, setId = 20)) }
        val nextReveal = reveal
        assertNotEquals(firstReveal, nextReveal)
        assertEquals(count + 1, events.size)
        update { questions.value = listOf(first) }
        assertEquals(nextReveal, reveal)
        update { questions.value = emptyList() }
        assertEquals(nextReveal, reveal)
        assertTrue(!waiting)
    }

    @Test
    fun equalNumericShellAndQuestionIdsRevealSeparatelyAndIgnoreForeignShells() {
        questions.value = listOf(question(7))
        mount()
        val firstReveal = reveal
        update { shell.value = command(7, "other") }
        assertEquals(firstReveal, reveal)
        update { shell.value = command(7) }
        val shellReveal = reveal
        assertNotEquals(firstReveal, shellReveal)
        val pending = shell.value!!
        update { shell.value = pending.copy(summary = "different display only") }
        assertEquals(shellReveal, reveal)
        update { shell.value = null }
        assertEquals(shellReveal, reveal)
        update { questions.value = listOf(question(7), question(8)) }
        assertNotEquals(shellReveal, reveal)
    }

    @Test
    fun minimizedArrivalStaysFoldedAndEveryExplicitExpansionGetsAnotherToken() {
        questions.value = listOf(question(1))
        mount()
        val initialReveal = reveal
        compose.onNodeWithContentDescription(text(R.string.interaction_minimize)).performClick()
        compose.waitForIdle()
        assertEquals(initialReveal, reveal)
        update { questions.value = listOf(question(1), question(2)) }
        val arrivalReveal = reveal
        assertNotEquals(initialReveal, arrivalReveal)
        compose.onNodeWithContentDescription(text(R.string.interaction_expand)).assertExists()
        compose.onNodeWithContentDescription(text(R.string.interaction_expand)).performClick()
        compose.waitForIdle()
        val expandedReveal = reveal
        assertNotEquals(arrivalReveal, expandedReveal)
        compose.onNodeWithText("Question 1?").assertExists()
        compose.onNodeWithText("1 / 2").assertExists()
        compose.onNodeWithContentDescription(text(R.string.interaction_minimize)).performClick()
        compose.waitForIdle()
        assertEquals(expandedReveal, reveal)
        compose.onNodeWithContentDescription(text(R.string.interaction_expand)).performClick()
        compose.waitForIdle()
        assertNotEquals(expandedReveal, reveal)
        verify(exactly = 0) { askUser.submitAll(any()) }
        verify(exactly = 0) { askUser.dismiss(any()) }
        verify(exactly = 0) { confirmations.resolve(any(), any(), any()) }
    }

    @Test
    fun navigationDoesNotRevealPendingRequestsButSameOwnerArrivalsStillDo() {
        questions.value = listOf(question(1), question(2, conversation = "d"))
        mount()
        val cReveal = reveal
        update { owner = "d" }
        val dReveal = reveal
        assertNull(dReveal)
        assertNotEquals(cReveal, dReveal)
        val count = events.size
        update { questions.value = questions.value + question(3) }
        assertEquals(dReveal, reveal)
        assertEquals(count, events.size)
        update { questions.value = questions.value + question(4, conversation = "d") }
        assertNotEquals(dReveal, reveal)
        val newRequestReveal = reveal
        assertEquals("d", (newRequestReveal as Pair<*, *>).first)
        update { owner = "c" }
        assertNull("Returning to an existing pending question is navigation", reveal)
        update { owner = "d" }
        assertNull("Returning to the other pending request does not replay its event", reveal)
        update { questions.value = questions.value + question(5, conversation = "d") }
        assertNotNull(reveal)
        assertNotEquals("A genuine new arrival gets a fresh serial", newRequestReveal, reveal)
        update { owner = "empty" }
        assertNull(reveal)
        assertTrue(!waiting)
    }

    @Test
    @Config(qualifiers = "w1280dp-h900dp-mdpi")
    fun expandedCardCentersLocksHorizontalDragAndCollapseRestoresScroll() {
        questions.value = listOf(question(1))
        mountTools()
        val row = compose.onNodeWithTag("conversation-tools-row")
        val viewport = row.fetchSemanticsNode().boundsInRoot
        val card = compose.onNodeWithTag("tools-slot:interaction").fetchSemanticsNode().boundsInRoot
        assertEquals("Expanded editor centers independently of following tools", viewport.center.x, card.left + 420f, 2f)
        compose.onAllNodes(hasScrollAction() and
            SemanticsMatcher.keyIsDefined(SemanticsProperties.HorizontalScrollAxisRange)).assertCountEquals(0)
        val locked = row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].value()
        row.performTouchInput { down(center); moveBy(Offset(-300f, 0f)); advanceEventTime(200); up() }
        compose.waitForIdle()
        assertEquals(locked, row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].value(), 0f)
        compose.onNodeWithContentDescription(text(R.string.interaction_minimize)).performClick()
        compose.waitForIdle()
        compose.onAllNodes(hasScrollAction() and
            SemanticsMatcher.keyIsDefined(SemanticsProperties.HorizontalScrollAxisRange)).assertCountEquals(1)
        compose.onNodeWithText("Worker 15 investigation").performScrollTo()
        compose.waitForIdle()
        assertTrue("The collapsed row has a real scroll range",
            row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].maxValue() > 0f)
        assertTrue("The collapsed row scrolls to the last capsule",
            row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].value() > 0f)
        compose.runOnIdle { questions.value = emptyList() }
        compose.waitForIdle()
        compose.onAllNodes(hasScrollAction() and
            SemanticsMatcher.keyIsDefined(SemanticsProperties.HorizontalScrollAxisRange)).assertCountEquals(1)
    }

    @Test
    @Config(qualifiers = "w1280dp-h900dp-mdpi")
    fun returningToExpandedParentCentersWithNoNewRevealToken() {
        questions.value = listOf(question(1))
        mountTools()
        update { owner = "child-0" }
        compose.onNodeWithText("Worker 15 investigation").performScrollTo()
        compose.waitForIdle()
        val row = compose.onNodeWithTag("conversation-tools-row")
        val before = row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].value()
        assertTrue("The child leaves a nonzero shared scroll offset", before > 0f)
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { owner = "c" }
        val offsets = mutableListOf<Float>()
        repeat(48) {
            compose.mainClock.advanceTimeByFrame()
            compose.waitForIdle()
            offsets += row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].value()
        }
        assertNull("Navigation must not manufacture a new request reveal", reveal)
        assertTrue("The existing scroll owner travels through intermediate offsets", offsets.any { it > 1f && it < before - 1f })
        assertEquals(0f, offsets.last(), 0f)
        val viewport = row.fetchSemanticsNode().boundsInRoot
        val card = compose.onNodeWithTag("tools-slot:interaction").fetchSemanticsNode().boundsInRoot
        assertEquals("The reopened 840dp editor reaches the viewport center", viewport.center.x, card.left + 420f, 2f)
        compose.onAllNodes(hasScrollAction() and
            SemanticsMatcher.keyIsDefined(SemanticsProperties.HorizontalScrollAxisRange)).assertCountEquals(0)
    }

    @Test
    @Config(qualifiers = "w1280dp-h900dp-mdpi")
    fun clearingExpandedCardReleasesCenteringDuringItsExistingExit() {
        questions.value = listOf(question(1))
        mountTools()
        val row = compose.onNodeWithTag("conversation-tools-row")
        val interaction = compose.onNodeWithTag("tools-slot:interaction")
        val todo = compose.onNodeWithTag("tools-slot:todo")
        val initialWidth = interaction.getUnclippedBoundsInRoot().let { it.right.value - it.left.value }
        val initialLeft = todo.getUnclippedBoundsInRoot().left.value
        val centeredInset = (row.fetchSemanticsNode().boundsInRoot.width - 24f - 840f) / 2f
        assertTrue("The wide fixture has a real centered offset", centeredInset > 100f)
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { questions.value = emptyList() }
        val samples = mutableListOf<Pair<Float, Float>>()
        repeat(48) {
            compose.mainClock.advanceTimeByFrame()
            compose.waitForIdle()
            samples += interaction.getUnclippedBoundsInRoot().let { it.right.value - it.left.value } to todo.getUnclippedBoundsInRoot().left.value
        }
        assertTrue("Centering must release before the outgoing card loses its occupied width: $samples",
            samples.any { (width, left) -> width >= initialWidth - 1f &&
                left < initialLeft - 2f && left > initialLeft - centeredInset + 2f })
        assertEquals("The empty interaction leaves no occupied width", 0f, samples.last().first, 0f)
        assertTrue("The remaining independent capsules still overflow the wide viewport",
            row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].maxValue() > 0f)
        assertEquals("Todo returns to the ordinary outer inset", 12f,
            samples.last().second - row.getUnclippedBoundsInRoot().left.value, 1f)
        assertEquals("Clearing the card must leave the logical scroll at zero", 0f,
            row.fetchSemanticsNode().config[SemanticsProperties.HorizontalScrollAxisRange].value(), 0f)
        compose.onAllNodes(hasScrollAction() and
            SemanticsMatcher.keyIsDefined(SemanticsProperties.HorizontalScrollAxisRange)).assertCountEquals(1)
    }

    @Test
    @Config(qualifiers = "w412dp-h800dp-mdpi")
    fun groupedParentReturnWithTodoAndQuestionNeverJumpsLeftBeforeMovingRight() {
        questions.value = listOf(question(1))
        // Cross the fit/overflow boundary with the same production row and grouped capsule.
        mountTools(grouped = true, width = 240)
        compose.onNodeWithContentDescription(text(R.string.interaction_minimize)).performClick()
        compose.waitForIdle()
        update { owner = "child-0" }
        val capsule = compose.onNodeWithTag("tools-slot:subagents:c")
        val identity = capsule.fetchSemanticsNode().id
        var previous = capsule.getUnclippedBoundsInRoot().left.value
        val childLeft = previous
        assertTrue("The grouped capsule starts centered", childLeft > 12f)
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { owner = "c" }
        val positions = mutableListOf<Float>()
        repeat(64) {
            compose.mainClock.advanceTimeByFrame()
            compose.waitForIdle()
            val left = capsule.getUnclippedBoundsInRoot().left.value
            positions += left
            assertEquals(identity, capsule.fetchSemanticsNode().id)
            assertTrue("Restoring both prefixes must not first jump left: $positions", left >= previous - 1f)
            previous = left
        }
        assertNull("The pending question is navigation, not a new reveal", reveal)
        assertTrue("The fixture overflows only after both prefixes return",
            compose.onNodeWithTag("conversation-tools-row").fetchSemanticsNode()
                .config[SemanticsProperties.HorizontalScrollAxisRange].maxValue() > 0f)
        assertTrue("The restored prefixes move the grouped capsule right", previous > childLeft + 20f)
    }

    private fun mountTools(grouped: Boolean = false, width: Int = 1200) {
        every { viewModel.askUser } returns askUser
        every { viewModel.shellConfirmation } returns confirmations
        every { askUser.requests } returns questions
        every { confirmations.pendingShellCommand } returns shell
        compose.setContent {
            MaterialTheme {
                ChatUserInteractionBarHost(viewModel, owner, false) { present, token, expanded, expansion, card ->
                    SideEffect { reveal = token }
                    ConversationToolsRow(owner, if (grouped && owner != "c") emptyList() else listOf(ConversationTodoItem("Task")),
                        List(16) { SubagentSummary("child-$it", "Worker $it investigation", "model", "c", "idle") },
                        {}, {}, modifier = Modifier.width(width.dp), hasInteraction = present, familyId = "c",
                        subagentCapsuleDisplayMode = if (grouped) SubagentCapsuleDisplayModes.GROUPED else
                            SubagentCapsuleDisplayModes.INDIVIDUAL,
                        interactionRevealKey = token, interactionExpanded = expanded,
                        interactionExpansion = expansion,
                    ) { modifier, height, inset, ready, animateEntry, transition ->
                        card(modifier, height, inset, ready, animateEntry, transition)
                    }
                }
            }
        }
        compose.waitForIdle()
    }

    private fun mount() {
        every { viewModel.askUser } returns askUser
        every { viewModel.shellConfirmation } returns confirmations
        every { askUser.requests } returns questions
        every { confirmations.pendingShellCommand } returns shell
        compose.setContent {
            MaterialTheme {
                ChatUserInteractionBarHost(viewModel, owner, false) { present, key, _, _, interaction ->
                    SideEffect {
                        waiting = present
                        reveal = key
                        if (events.isEmpty() || events.last() != key) events += key
                    }
                    Box(Modifier.width(400.dp)) {
                        interaction(Modifier, {}, {}, true, true, { _, _ -> })
                    }
                }
            }
        }
        compose.waitForIdle()
    }

    private fun update(change: () -> Unit) {
        compose.runOnIdle(change)
        compose.waitForIdle()
    }

    private fun text(id: Int) = RuntimeEnvironment.getApplication().getString(id)

    private fun question(id: Long, conversation: String = "c", setId: Long? = null) =
        AskUserController.Request(
            id = id, conversationId = conversation, question = "Question $id?", options = emptyList(),
            allowMultiple = false, blocking = false, setId = setId,
        )

    private fun command(id: Long, conversation: String = "c") =
        ShellConfirmationController.PendingShellCommand(
            id = id, conversationId = conversation, server = "test-server", summary = "echo test-$id",
            deferred = CompletableDeferred(),
        )
}
