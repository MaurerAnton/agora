package com.newoether.agora.ui.remote

import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import com.newoether.agora.remote.*
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class RemoteConversationSettingsTest {
    @get:Rule val compose = createComposeRule()
    private val model = RemoteModel("native", "Native", reasoningEfforts = listOf("none", "high", "ultra"),
        serviceTiers = listOf(RemoteServiceTier("priority", "Priority")))
    private fun state() = RemoteState(session = RemoteSession("session", "Task", "/native", 1),
        models = listOf(model), runtime = RemoteRuntime("notLoaded", model = "native"))
    private val slider = SemanticsMatcher.keyIsDefined(SemanticsProperties.ProgressBarRangeInfo)
    private fun assertUnknown() {
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo.Indeterminate))
    }

    @Test fun unknownThinkingKeepsOriginalSliderAndUltraUsesNativeReadBack() {
        var state by mutableStateOf(state())
        val changes = mutableListOf<String>()
        compose.setContent {
            MaterialTheme { Column { RemoteThinkingControl(state, true) { changes += it } } }
        }
        assertUnknown()
        compose.onNodeWithText("Ultra").assertDoesNotExist()
        compose.onNode(slider).performTouchInput { click(Offset(width * 0.9f, height * 0.5f)) }
        compose.runOnIdle { assertEquals(listOf("ultra"), changes); assertNull(state.selectedEffort) }
        compose.onNodeWithText("Ultra").assertExists()
        compose.runOnIdle { state = state.copy(runtime = state.runtime!!.copy(effort = "ultra"), settingsRevision = 1) }
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(2f, 0f..2f, 1)))
        compose.onNodeWithText("Ultra").assertExists()
    }

    @Test fun unknownTierAllowsExplicitDefaultThenFailureRestoresUnselectedPanel() {
        var state by mutableStateOf(state())
        val changes = mutableListOf<String?>()
        compose.setContent {
            MaterialTheme { Column {
                RemoteServiceTierControl(state, true, mapOf("priority" to "Priority")) { changes += it }
            } }
        }
        assertUnknown()
        compose.onNodeWithText("Default").assertDoesNotExist()
        compose.onNode(slider).performTouchInput { click(Offset(width * 0.1f, height * 0.5f)) }
        compose.runOnIdle { assertEquals(listOf<String?>(null), changes); assertFalse(state.serviceTierKnown) }
        compose.runOnIdle { state = state.copy(settingsRevision = 1) }
        assertUnknown()
        compose.onNodeWithText("Default").assertDoesNotExist()
        compose.onNode(slider).performTouchInput { click(Offset(width * 0.9f, height * 0.5f)) }
        compose.runOnIdle { assertEquals(listOf(null, "priority"), changes); assertNull(state.selectedServiceTier) }
        compose.runOnIdle {
            state = state.copy(runtime = state.runtime!!.copy(serviceTier = "priority", serviceTierKnown = true), settingsRevision = 2)
        }
        compose.onNodeWithText("Priority").assertExists()
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(1f, 0f..1f, 0)))
    }

    @Test fun unknownSingletonThinkingAllowsGestureAndAccessibilityWithoutInventingCurrentValue() {
        var state by mutableStateOf(state().copy(models = listOf(model.copy(reasoningEfforts = listOf("ultra")))))
        val changes = mutableListOf<String>()
        compose.setContent {
            MaterialTheme { Column { RemoteThinkingControl(state, true) { changes += it } } }
        }
        assertUnknown()
        compose.onNode(slider).assertIsEnabled()
        compose.onNodeWithText("Ultra").assertDoesNotExist()
        compose.runOnIdle { assertTrue(changes.isEmpty()); assertNull(state.selectedEffort) }
        compose.onNode(slider).performTouchInput {
            down(Offset(width * 0.9f, height * 0.5f))
            moveTo(Offset(width * 0.6f, height * 0.5f))
        }
        compose.runOnIdle { assertTrue(changes.isEmpty()); assertNull(state.selectedEffort) }
        compose.onNodeWithText("Ultra").assertExists()
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(0f, 0f..1f, 0)))
        compose.onNode(slider).performTouchInput { up() }
        compose.runOnIdle { assertEquals(listOf("ultra"), changes); assertNull(state.selectedEffort) }
        compose.runOnIdle { state = state.copy(settingsRevision = 1) }
        assertUnknown()
        compose.onNodeWithText("Ultra").assertDoesNotExist()
        compose.onNode(slider).performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(1f)) }
        compose.runOnIdle { assertEquals(listOf("ultra", "ultra"), changes); assertNull(state.selectedEffort) }
        compose.onNodeWithText("Ultra").assertExists()
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(0f, 0f..1f, 0)))
        compose.runOnIdle { state = state.copy(runtime = state.runtime!!.copy(effort = "ultra"), settingsRevision = 2) }
        compose.onNode(slider).assertIsNotEnabled()
        compose.onNodeWithText("Ultra").assertExists()
        compose.onNode(slider).performTouchInput { click(Offset(width * 0.9f, height * 0.5f)) }
        compose.runOnIdle { assertEquals(listOf("ultra", "ultra"), changes) }
    }

    @Test fun unknownDefaultOnlyTierAllowsGestureAndAccessibilityUntilNativeReadBack() {
        var state by mutableStateOf(state().copy(models = listOf(model.copy(serviceTiers = emptyList()))))
        val changes = mutableListOf<String?>()
        compose.setContent {
            MaterialTheme { Column { RemoteServiceTierControl(state, true, emptyMap()) { changes += it } } }
        }
        assertUnknown()
        compose.onNode(slider).assertIsEnabled()
        compose.onNodeWithText("Default").assertDoesNotExist()
        compose.runOnIdle { assertTrue(changes.isEmpty()); assertFalse(state.serviceTierKnown) }
        compose.onNode(slider).performTouchInput {
            down(Offset(width * 0.9f, height * 0.5f))
            moveTo(Offset(width * 0.6f, height * 0.5f))
        }
        compose.runOnIdle { assertTrue(changes.isEmpty()); assertFalse(state.serviceTierKnown) }
        compose.onNodeWithText("Default").assertExists()
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(0f, 0f..1f, 0)))
        compose.onNode(slider).performTouchInput { up() }
        compose.runOnIdle { assertEquals(listOf<String?>(null), changes); assertFalse(state.serviceTierKnown) }
        compose.runOnIdle { state = state.copy(settingsRevision = 1) }
        assertUnknown()
        compose.onNodeWithText("Default").assertDoesNotExist()
        compose.onNode(slider).performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(1f)) }
        compose.runOnIdle { assertEquals(listOf<String?>(null, null), changes); assertFalse(state.serviceTierKnown) }
        compose.onNodeWithText("Default").assertExists()
        compose.onNode(slider).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(0f, 0f..1f, 0)))
        compose.runOnIdle {
            state = state.copy(runtime = state.runtime!!.copy(serviceTierKnown = true), settingsRevision = 2)
        }
        compose.onNode(slider).assertIsNotEnabled()
        compose.onNodeWithText("Default").assertExists()
        compose.onNode(slider).performTouchInput { click(Offset(width * 0.9f, height * 0.5f)) }
        compose.runOnIdle { assertEquals(listOf<String?>(null, null), changes) }
    }

    @Test fun disabledUnknownSingletonControlsCannotWrite() {
        val state = state().copy(models = listOf(model.copy(
            reasoningEfforts = listOf("ultra"), serviceTiers = emptyList(),
        )))
        val effortChanges = mutableListOf<String>()
        val tierChanges = mutableListOf<String?>()
        compose.setContent {
            MaterialTheme { Column {
                RemoteThinkingControl(state, false) { effortChanges += it }
                RemoteServiceTierControl(state, false, emptyMap()) { tierChanges += it }
            } }
        }
        val sliders = compose.onAllNodes(slider)
        sliders.assertCountEquals(2)
        for (index in 0..1) {
            sliders[index].assertIsNotEnabled().assert(SemanticsMatcher.expectValue(
                SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo.Indeterminate))
                .performTouchInput { click(Offset(width * 0.9f, height * 0.5f)) }
        }
        compose.runOnIdle { assertTrue(effortChanges.isEmpty()); assertTrue(tierChanges.isEmpty()) }
        compose.onNodeWithText("Ultra").assertDoesNotExist()
        compose.onNodeWithText("Default").assertDoesNotExist()
    }

    @Test fun optimisticChoicesRemainEditableAndFailureRestoresConfirmedValues() {
        var state by mutableStateOf(state().copy(deviceId = "device",
            runtime = RemoteRuntime("idle", model = "native", effort = "high", serviceTier = "default")))
        val effortChanges = mutableListOf<String>()
        val tierChanges = mutableListOf<String?>()
        compose.setContent {
            MaterialTheme { Column {
                RemoteThinkingControl(state, state.canEditSettings) { effortChanges += it }
                RemoteServiceTierControl(state, state.canEditSettings, mapOf("priority" to "Priority")) { tierChanges += it }
            } }
        }
        compose.runOnIdle {
            state = state.copy(optimisticSettings = mapOf(state.owner!! to RemoteSettings(
                effort = "ultra", serviceTier = "priority", updateServiceTier = true)))
        }
        compose.onNodeWithText("Ultra").assertExists()
        compose.onNodeWithText("Priority").assertExists()
        val sliders = compose.onAllNodes(slider)
        sliders[0].assertIsEnabled().performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(1f)) }
        sliders[1].assertIsEnabled().performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(0f)) }
        compose.runOnIdle {
            assertEquals(listOf("high"), effortChanges)
            assertEquals(listOf<String?>(null), tierChanges)
            state = state.copy(optimisticSettings = emptyMap(), settingsRevision = 1)
        }
        sliders[0].assertIsEnabled().assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(1f, 0f..2f, 1)))
        sliders[1].assertIsEnabled().assert(SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo, ProgressBarRangeInfo(0f, 0f..1f, 0)))
        compose.onNodeWithText("Default").assertExists()
    }

    @Test fun retiredThinkingStaysUnselectedAndDisabledPanelCannotWrite() {
        val state = state().copy(runtime = state().runtime!!.copy(effort = "retired"))
        val changes = mutableListOf<String>()
        compose.setContent {
            MaterialTheme { Column { RemoteThinkingControl(state, false) { changes += it } } }
        }
        assertUnknown()
        compose.onNodeWithText("retired").assertDoesNotExist()
        compose.onNode(slider).assertIsNotEnabled().performTouchInput { click(Offset(width * 0.9f, height * 0.5f)) }
        compose.runOnIdle { assertTrue(changes.isEmpty()) }
        assertUnknown()
    }
}
