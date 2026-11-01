package com.newoether.agora.ui.settings

import android.app.Application
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.getUnclippedBoundsInRoot
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.unit.dp
import com.newoether.agora.ui.motion.AgoraMotionPolicy
import com.newoether.agora.ui.motion.LocalAgoraMotionPolicy
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = Application::class, qualifiers = "w400dp-h800dp-mdpi")
class SettingsScrollRetentionTest {
    @get:Rule val compose = createComposeRule()

    private val retention = SettingsScrollRetention()
    private lateinit var scope: CoroutineScope

    @Composable
    private fun Host(reducedMotion: Boolean = false, content: @Composable () -> Unit) {
        scope = rememberCoroutineScope()
        CompositionLocalProvider(
            LocalSettingsScrollRetention provides retention,
            LocalAgoraMotionPolicy provides AgoraMotionPolicy(reduceMotion = reducedMotion),
        ) {
            MaterialTheme { Box(Modifier.size(400.dp, 600.dp)) { content() } }
        }
    }

    @Composable
    private fun ColumnPage(title: String, rows: Int = 30) {
        CollapsingSettingsScaffold(title = title, onBack = {}) {
            repeat(rows) { Text("Row $it", modifier = Modifier.height(48.dp)) }
        }
    }

    @Test
    fun returningRestoresColumnAndTitleWithoutRevivingSaveableEditorDraft() {
        var route by mutableStateOf("parent")
        lateinit var draft: MutableState<String>
        compose.setContent {
            Host {
                GuardedAnimatedContent(route, forward = route == "editor") { page ->
                    if (page == "parent") {
                        ColumnPage("Parent")
                    } else {
                        draft = rememberSaveable { mutableStateOf("saved") }
                        Text(draft.value)
                    }
                }
            }
        }
        compose.runOnIdle {
            scope.launch { retention.scrollState(listOf("parent"))!!.scrollTo(37) }
        }
        val titleTop = compose.onNodeWithText("Parent").getUnclippedBoundsInRoot().top
        compose.runOnIdle { route = "editor" }
        compose.runOnIdle { draft.value = "discarded" }
        compose.runOnIdle { route = "parent" }
        compose.runOnIdle { assertEquals(37, retention.scrollState(listOf("parent"))!!.value) }
        assertEquals(titleTop, compose.onNodeWithText("Parent").getUnclippedBoundsInRoot().top)
        compose.runOnIdle { route = "editor" }
        compose.runOnIdle { assertEquals("saved", draft.value) }
    }

    @Test
    fun lazyScaffoldRestoresIndexAndOffsetWithReducedMotion() {
        var route by mutableStateOf("list")
        compose.setContent {
            Host(reducedMotion = true) {
                GuardedAnimatedContent(route, forward = route == "child") { page ->
                    if (page == "list") {
                        CollapsingSettingsLazyScaffold(title = "Lazy", onBack = {}) {
                            items(40, key = { "row:$it" }) { index ->
                                Text("Row $index", modifier = Modifier.height(48.dp))
                            }
                        }
                    } else {
                        Text("Child")
                    }
                }
            }
        }
        compose.runOnIdle {
            scope.launch { retention.lazyListState(listOf("list"))!!.scrollToItem(12, 23) }
        }
        val titleTop = compose.onNodeWithText("Lazy").getUnclippedBoundsInRoot().top
        compose.runOnIdle { route = "child" }
        compose.runOnIdle { route = "list" }
        compose.runOnIdle {
            val state = retention.lazyListState(listOf("list"))!!
            assertEquals(12, state.firstVisibleItemIndex)
            assertEquals(23, state.firstVisibleItemScrollOffset)
        }
        assertEquals(titleTop, compose.onNodeWithText("Lazy").getUnclippedBoundsInRoot().top)
    }

    @Test
    fun nestedProviderAndModelPathsUseStableIdentityRatherThanConfigSnapshot() {
        data class Route(val id: String, val revision: Int)
        var provider by mutableStateOf("provider:A")
        var model by mutableStateOf(Route("model:1", 0))
        compose.setContent {
            Host {
                GuardedAnimatedContent(provider, forward = true) {
                    GuardedAnimatedContent(model, forward = true, contentKey = { it.id }) {
                        ColumnPage("Model")
                    }
                }
            }
        }
        val first = listOf("provider:A", "model:1")
        compose.runOnIdle { scope.launch { retention.scrollState(first)!!.scrollTo(325) } }
        compose.runOnIdle { model = Route("model:2", 0) }
        compose.runOnIdle {
            assertEquals(0, retention.scrollState(listOf("provider:A", "model:2"))!!.value)
        }
        compose.runOnIdle { provider = "provider:B" }
        compose.runOnIdle { model = Route("model:1", 1) }
        compose.runOnIdle {
            assertEquals(0, retention.scrollState(listOf("provider:B", "model:1"))!!.value)
        }
        compose.runOnIdle { provider = "provider:A" }
        compose.runOnIdle { assertEquals(325, retention.scrollState(first)!!.value) }
    }

    @Test
    fun returningAfterContentShrinksUsesOrdinaryScrollClamping() {
        var route by mutableStateOf("parent")
        var rows by mutableIntStateOf(40)
        compose.setContent {
            Host {
                GuardedAnimatedContent(route, forward = route == "child") { page ->
                    if (page == "parent") ColumnPage("Parent", rows) else Text("Child")
                }
            }
        }
        compose.runOnIdle {
            scope.launch { retention.scrollState(listOf("parent"))!!.scrollTo(1_200) }
        }
        compose.runOnIdle { route = "child" }
        compose.runOnIdle { rows = 12 }
        compose.runOnIdle { route = "parent" }
        compose.runOnIdle {
            val state = retention.scrollState(listOf("parent"))!!
            assertTrue(state.maxValue < 1_200)
            assertEquals(state.maxValue, state.value)
        }
    }

    @Test
    fun lazyContentShrinkingClampsToTheRemainingItems() {
        var route by mutableStateOf("list")
        var rows by mutableIntStateOf(40)
        compose.setContent {
            Host {
                GuardedAnimatedContent(route, forward = route == "child") { page ->
                    if (page == "list") {
                        CollapsingSettingsLazyScaffold(title = "Lazy", onBack = {}) {
                            items(rows, key = { "row:$it" }) { index ->
                                Text("Row $index", modifier = Modifier.height(48.dp))
                            }
                        }
                    } else {
                        Text("Child")
                    }
                }
            }
        }
        compose.runOnIdle {
            scope.launch { retention.lazyListState(listOf("list"))!!.scrollToItem(30, 23) }
        }
        compose.runOnIdle { route = "child" }
        compose.runOnIdle { rows = 20 }
        compose.runOnIdle { route = "list" }
        compose.runOnIdle {
            val state = retention.lazyListState(listOf("list"))!!
            assertEquals(rows + 2, state.layoutInfo.totalItemsCount)
            assertTrue(state.firstVisibleItemIndex < rows + 2)
            assertTrue(state.firstVisibleItemScrollOffset >= 0)
            assertTrue(state.layoutInfo.visibleItemsInfo.isNotEmpty())
        }
    }

    @Test
    fun interruptedBackUsesANewOwnerAndCannotBeOverwrittenByTheOutgoingComposition() {
        var route by mutableStateOf("parent")
        compose.setContent {
            Host {
                GuardedAnimatedContent(route, forward = route == "child") { page ->
                    if (page == "parent") ColumnPage("Parent") else Text("Child")
                }
            }
        }
        compose.runOnIdle {
            scope.launch { retention.scrollState(listOf("parent"))!!.scrollTo(420) }
        }
        lateinit var outgoing: ScrollState
        compose.runOnIdle { outgoing = retention.scrollState(listOf("parent"))!! }
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { route = "child" }
        // The effect changes slots; BoxWithConstraints composes those slots during layout.
        repeat(2) {
            compose.mainClock.advanceTimeByFrame()
            compose.waitForIdle()
        }
        compose.mainClock.advanceTimeBy(64)
        compose.waitForIdle()
        compose.onNodeWithText("Child").assertExists()
        compose.onNodeWithText("Parent").assertExists()
        compose.runOnIdle { route = "parent" }
        repeat(2) {
            compose.mainClock.advanceTimeByFrame()
            compose.waitForIdle()
        }
        compose.mainClock.advanceTimeBy(64)
        compose.waitForIdle()
        compose.onNodeWithText("Parent").assertExists()
        compose.onNodeWithText("Child").assertExists()
        compose.runOnIdle {
            val returning = retention.scrollState(listOf("parent"))!!
            assertNotSame(outgoing, returning)
            assertEquals(420, returning.value)
            scope.launch { returning.scrollTo(210) }
        }
        compose.mainClock.autoAdvance = true
        compose.waitForIdle()
        compose.runOnIdle { route = "child" }
        compose.runOnIdle { route = "parent" }
        compose.runOnIdle { assertEquals(210, retention.scrollState(listOf("parent"))!!.value) }
    }

    @Test
    fun narrowAndWideCategoryPathsShareOnlyTheScrollPosition() {
        var wide by mutableStateOf(false)
        var generation by mutableIntStateOf(0)
        compose.setContent {
            Host {
                if (wide) {
                    CompositionLocalProvider(LocalSettingsScrollPath provides listOf("models")) {
                        ColumnPage("Models")
                    }
                } else {
                    androidx.compose.runtime.key(generation) {
                        GuardedAnimatedContent("models", forward = true) { ColumnPage("Models") }
                    }
                }
            }
        }
        compose.runOnIdle {
            scope.launch { retention.scrollState(listOf("models"))!!.scrollTo(340) }
        }
        compose.runOnIdle { wide = true }
        compose.runOnIdle { assertEquals(340, retention.scrollState(listOf("models"))!!.value) }
        compose.runOnIdle { wide = false; generation++ }
        compose.runOnIdle { assertEquals(340, retention.scrollState(listOf("models"))!!.value) }
    }

    @Test
    fun retainedOwnersAreBoundedAndSeparateScrollKindsAndSessions() {
        val first = ScrollState(18)
        retention.retain(listOf("first"), first)
        repeat(63) { retention.retain(listOf("page:$it"), ScrollState(it)) }
        assertEquals(first, retention.scrollState(listOf("first")))
        retention.retain(listOf("last"), ScrollState(99))
        assertNull(retention.scrollState(listOf("page:0")))
        assertEquals(first, retention.scrollState(listOf("first")))
        assertNull(retention.lazyListState(listOf("first")))
        assertNull(SettingsScrollRetention().scrollState(listOf("first")))
    }
}
