package com.newoether.agora.ui.settings

import android.app.Application
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.unit.dp
import com.newoether.agora.R
import com.newoether.agora.ui.motion.AgoraMotionPolicy
import com.newoether.agora.ui.motion.LocalAgoraMotionPolicy
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = Application::class, qualifiers = "en-w400dp-h900dp-mdpi")
class SettingsSubagentLimitsTest {
    @get:Rule val compose = createComposeRule()

    private var total by mutableStateOf(16)
    private var concurrent by mutableStateOf(4)
    private val totalChanges = mutableListOf<Int>()
    private val concurrentChanges = mutableListOf<Int>()

    @Composable
    private fun Host(content: @Composable () -> Unit) {
        CompositionLocalProvider(
            LocalAgoraMotionPolicy provides AgoraMotionPolicy(reduceMotion = true),
        ) {
            MaterialTheme { Box(Modifier.size(400.dp, 800.dp)) { content() } }
        }
    }

    private fun showPage() = compose.setContent {
        Host {
            Page(
                maxTotal = total,
                maxConcurrent = concurrent,
                onTotalChange = { totalChanges += it; total = it },
                onConcurrentChange = { concurrentChanges += it; concurrent = it },
            )
        }
    }

    @Composable
    private fun Page(
        maxTotal: Int,
        maxConcurrent: Int,
        onTotalChange: (Int) -> Unit,
        onConcurrentChange: (Int) -> Unit,
    ) {
        SettingsSubagentContent(
            subagentToolsEnabled = false,
            subagentEnabledModels = null,
            subagentRecommendedModel = null,
            enabledModels = emptySet(),
            modelAliases = emptyMap(),
            customProviders = emptyList(),
            onSubagentToolsEnabledChange = {},
            onAddModels = {},
            onRemoveModel = {},
            onRecommendedModelChange = {},
            onBack = {},
            subagentMaxTotal = maxTotal,
            subagentMaxConcurrent = maxConcurrent,
            onSubagentMaxTotalChange = onTotalChange,
            onSubagentMaxConcurrentChange = onConcurrentChange,
        )
    }

    @Test
    fun disabledDelegationKeepsBothLimitsAvailableAndSavesAccessibleChoicesWithoutSaveAction() {
        total = 8
        showPage()
        compose.onNodeWithTag("subagent-max-total").performScrollTo().assertIsEnabled()
            .assert(range(8f, 1f..16f, 14))
            .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(16f)) }
        compose.onNodeWithTag("subagent-max-concurrent").performScrollTo().assertIsEnabled()
            .assert(range(4f, 1f..16f, 14))
            .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(16f)) }
        compose.onNodeWithTag("subagent-capsule-display").assertIsEnabled()
        compose.onNodeWithText("Save").assertDoesNotExist()
        compose.runOnIdle {
            assertEquals(listOf(16), totalChanges)
            assertEquals(listOf(16), concurrentChanges)
        }
    }

    @Test
    fun loweringTotalChangesConcurrentRangeWithoutWritingFromComposition() {
        showPage()
        compose.runOnIdle { total = 3 }
        compose.onNodeWithTag("subagent-max-concurrent").performScrollTo().assertIsEnabled()
            .assert(range(3f, 1f..3f, 1))
        compose.runOnIdle {
            assertTrue(totalChanges.isEmpty())
            assertTrue(concurrentChanges.isEmpty())
        }
        compose.onNodeWithTag("subagent-max-concurrent")
            .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(2f)) }
        compose.runOnIdle { assertEquals(listOf(2), concurrentChanges) }
        compose.runOnIdle { total = 1 }
        compose.onNodeWithTag("subagent-max-concurrent").assertIsNotEnabled()
            .assert(range(1f, 1f..1f, 0))
        compose.runOnIdle { total = 8 }
        compose.onNodeWithTag("subagent-max-concurrent").assertIsEnabled()
            .assert(range(2f, 1f..8f, 6))
        compose.runOnIdle { assertEquals(listOf(2), concurrentChanges) }
    }

    @Test
    fun singletonConcurrentSliderHasValidRangeAndNeverWritesOnTouch() {
        total = 1
        concurrent = 1
        showPage()
        compose.onNodeWithTag("subagent-max-total").performScrollTo().assertIsEnabled()
            .assert(range(1f, 1f..16f, 14))
        compose.onNodeWithTag("subagent-max-concurrent").performScrollTo().assertIsNotEnabled()
            .assert(range(1f, 1f..1f, 0))
            .performTouchInput { click(center) }
        compose.runOnIdle {
            assertTrue(totalChanges.isEmpty())
            assertTrue(concurrentChanges.isEmpty())
        }
    }

    @Test
    fun draggingWaitsForReleaseAndIgnoresOldPersistenceUntilTheFinalValueArrives() {
        var persisted by mutableStateOf(8)
        val changes = mutableListOf<Int>()
        compose.setContent {
            Host {
                SubagentLimitSlider(
                    value = persisted,
                    valueRange = 1..16,
                    title = R.string.subagents_max_total,
                    description = R.string.subagents_max_total_desc,
                    onValueChange = { changes += it },
                    modifier = Modifier.testTag("limit"),
                )
            }
        }
        val slider = compose.onNodeWithTag("limit")
        slider.performTouchInput {
            down(center)
            moveTo(Offset(width + 32f, center.y), delayMillis = 200)
        }
        compose.runOnIdle {
            assertTrue(changes.isEmpty())
            persisted = 12
        }
        slider.assert(range(16f, 1f..16f, 14))
        slider.performTouchInput { up() }
        compose.runOnIdle { assertEquals(listOf(16), changes) }
        compose.runOnIdle { persisted = 10 }
        slider.assert(range(16f, 1f..16f, 14))
        compose.runOnIdle { persisted = 16 }
        slider.assert(range(16f, 1f..16f, 14))
        compose.runOnIdle { persisted = 8 }
        slider.assert(range(8f, 1f..16f, 14))
    }

    @Test
    fun unchangedAccessibleChoiceDoesNotWrite() {
        showPage()
        compose.onNodeWithTag("subagent-max-total").performScrollTo()
            .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(!it(16f)) }
        compose.onNodeWithTag("subagent-max-concurrent").performScrollTo()
            .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(!it(4f)) }
        compose.runOnIdle {
            assertTrue(totalChanges.isEmpty())
            assertTrue(concurrentChanges.isEmpty())
        }
    }

    private fun range(current: Float, range: ClosedFloatingPointRange<Float>, steps: Int) =
        SemanticsMatcher.expectValue(
            SemanticsProperties.ProgressBarRangeInfo,
            ProgressBarRangeInfo(current, range, steps),
        )
}
