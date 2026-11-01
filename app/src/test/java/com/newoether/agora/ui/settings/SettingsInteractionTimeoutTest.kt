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
import androidx.compose.ui.platform.LocalContext
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
class SettingsInteractionTimeoutTest {
    @get:Rule val compose = createComposeRule()

    private var enabled by mutableStateOf(true)
    private var minutes by mutableStateOf(5)
    private val changes = mutableListOf<Int>()
    private val enableChanges = mutableListOf<Boolean>()
    private val presets = listOf(1, 2, 5, 10, 15, 30, 60)
    private lateinit var labels: Map<Int, String>
    private lateinit var timeoutTitle: String
    private lateinit var enableTitle: String
    private lateinit var saveTitle: String
    private val sliderMatcher = SemanticsMatcher.keyIsDefined(SemanticsProperties.ProgressBarRangeInfo)

    @Composable
    private fun Host(content: @Composable () -> Unit) {
        val context = LocalContext.current
        labels = presets.associateWith { context.getString(R.string.ask_user_timeout_minutes, it) }
        timeoutTitle = context.getString(R.string.ask_user_timeout)
        enableTitle = context.getString(R.string.ask_user_enable)
        saveTitle = context.getString(R.string.provider_save)
        CompositionLocalProvider(
            LocalSettingsScrollRetention provides null,
            LocalAgoraMotionPolicy provides AgoraMotionPolicy(reduceMotion = true),
        ) {
            MaterialTheme { Box(Modifier.size(400.dp, 800.dp)) { content() } }
        }
    }

    private fun showPage() = compose.setContent {
        Host {
            SettingsInteractionContent(
                askUserEnabled = enabled,
                timeoutMinutes = minutes,
                onEnabledChange = { enableChanges += it; enabled = it },
                onTimeoutChange = { changes += it; minutes = it },
                onBack = {},
            )
        }
    }

    private fun slider() = compose.onNode(sliderMatcher)
    private fun range(index: Int) = SemanticsMatcher.expectValue(
        SemanticsProperties.ProgressBarRangeInfo,
        ProgressBarRangeInfo(index.toFloat(), 0f..6f, 5),
    )

    @Test
    fun disabledAskUserKeepsSevenPositionSliderAvailableAtFiveMinuteDefaultWithoutSave() {
        enabled = false
        showPage()
        compose.onAllNodes(sliderMatcher).assertCountEquals(1)
        slider().performScrollTo().assertIsEnabled().assert(range(2))
            .assert(hasContentDescription(timeoutTitle))
        compose.onNodeWithText(labels.getValue(5)).assertExists()
        compose.onNodeWithText(saveTitle).assertDoesNotExist()
        slider().performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(3f)) }
        compose.onNodeWithText(labels.getValue(10)).assertExists()
        compose.runOnIdle {
            assertEquals(listOf(10), changes)
            assertEquals(10, minutes)
            assertTrue(enableChanges.isEmpty())
            assertTrue(!enabled)
        }
    }

    @Test
    fun everyTrackPositionMapsToItsMinutePresetAndLocalizedValue() {
        showPage()
        for ((index, expectedMinutes) in presets.withIndex()) {
            slider().performScrollTo()
                .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(it(index.toFloat())) }
            slider().assert(range(index))
            compose.onNodeWithText(labels.getValue(expectedMinutes)).assertExists()
            compose.runOnIdle { assertEquals(expectedMinutes, minutes) }
        }
        compose.runOnIdle {
            assertEquals(presets, changes)
            assertTrue(enableChanges.isEmpty())
        }
    }

    @Test
    fun togglingAskUserRetainsChosenTimeoutAndNeverWritesTimeoutAsASideEffect() {
        minutes = 30
        showPage()
        compose.onNodeWithText(enableTitle).performClick()
        slider().performScrollTo().assertIsEnabled().assert(range(5))
        compose.onNodeWithText(labels.getValue(30)).assertExists()
        compose.onNodeWithText(enableTitle).performScrollTo().performClick()
        slider().assertIsEnabled().assert(range(5))
        compose.runOnIdle {
            assertEquals(listOf(false, true), enableChanges)
            assertTrue(changes.isEmpty())
            assertEquals(30, minutes)
        }
    }

    @Test
    fun sameAccessibleChoiceDoesNotPersistAgain() {
        showPage()
        slider().performScrollTo()
            .performSemanticsAction(SemanticsActions.SetProgress) { assertTrue(!it(2f)) }
        compose.runOnIdle { assertTrue(changes.isEmpty()) }
    }

    @Test
    fun invalidPersistedMinuteDisplaysDefaultWithoutWritingFromComposition() {
        minutes = 7
        showPage()
        slider().performScrollTo().assertIsEnabled().assert(range(2))
        compose.onNodeWithText(labels.getValue(5)).assertExists()
        compose.runOnIdle {
            assertEquals(7, minutes)
            assertTrue(changes.isEmpty())
        }
    }

    @Test
    fun dragWritesOnlyOnReleaseAndIgnoresStaleEchoesUntilFinalPresetIsPersisted() {
        compose.setContent {
            Host { InteractionTimeoutSlider(value = minutes, onValueChange = { changes += it }) }
        }
        slider().performTouchInput {
            down(center)
            moveTo(Offset(width.toFloat() - 1f, center.y), delayMillis = 200)
        }
        compose.runOnIdle {
            assertTrue(changes.isEmpty())
            minutes = 2
        }
        slider().assert(range(6))
        compose.onNodeWithText(labels.getValue(60)).assertExists()
        slider().performTouchInput { up() }
        compose.runOnIdle { assertEquals(listOf(60), changes) }
        compose.runOnIdle { minutes = 1 }
        slider().assert(range(6))
        compose.runOnIdle { minutes = 60 }
        slider().assert(range(6))
        compose.runOnIdle { minutes = 15 }
        slider().assert(range(4))
        compose.runOnIdle { assertEquals(listOf(60), changes) }
    }
}
