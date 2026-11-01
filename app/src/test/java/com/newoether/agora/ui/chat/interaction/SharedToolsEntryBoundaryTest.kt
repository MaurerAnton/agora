package com.newoether.agora.ui.chat.interaction

import android.graphics.Bitmap
import android.graphics.Canvas
import android.view.View
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithTag
import com.newoether.agora.model.ConversationTodoItem
import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import kotlin.math.ceil

/** Exercise shared entrance cancellation and the loaded-empty/live boundary on the real row. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class, qualifiers = "w412dp-h800dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SharedToolsEntryBoundaryTest {
    @get:Rule val compose = createComposeRule()
    private var owner by mutableStateOf("old")
    private var ready by mutableStateOf(true)
    private var hasTodo by mutableStateOf(true)
    private var inset = 0f
    private lateinit var view: View

    @Test fun displayedOutgoingRowKeepsItsInsetWhileItStillPaints() {
        mount()
        val restingInset = inset
        assertTrue(restingInset > 0f)
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { owner = "new" }
        frame()
        assertTrue("The outgoing capsule still paints", strongestRed() > 0)
        assertTrue("Outgoing paint retains its own message clearance", inset > restingInset * 0.5f)
        repeat(40) { frame() }
        assertEquals(restingInset, inset, 0.5f)
    }

    @Test fun switchingDuringHiddenEntryDoesNotFlashTheOutgoingRow() {
        ready = false
        mount()
        compose.mainClock.autoAdvance = false
        assertEquals(0, strongestRed())
        compose.runOnIdle { owner = "new" }
        repeat(4) {
            frame()
            assertEquals("Unadmitted outgoing content stays hidden", 0, strongestRed())
            assertEquals(0f, inset, 0.5f)
        }
    }

    @Test fun switchingDuringPartialEntryNeverPromotesItsOpacity() {
        ready = false
        mount()
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { ready = true }
        var strength = 0
        repeat(20) { if (strength < 40) { frame(); strength = strongestRed() } }
        assertTrue("Sample a partially visible shared entrance", strength in 40..150)
        compose.runOnIdle { owner = "new"; ready = false }
        frame()
        val outgoing = strongestRed()
        // One frame can advance before the replacement effect cancels the entering coroutine.
        assertTrue("Replacement keeps partial opacity rather than jumping to settled paint: $strength -> $outgoing",
            outgoing <= strength + 64 && outgoing < 180)
    }

    @Test fun loadedEmptyCompletesBeforeALiveTodoArrives() {
        ready = false
        hasTodo = false
        mount()
        compose.mainClock.autoAdvance = false
        compose.runOnIdle { ready = true }
        repeat(4) { frame() }
        compose.runOnIdle { hasTodo = true }
        val widths = mutableListOf<Int>()
        repeat(32) {
            frame()
            widths += compose.onNodeWithTag("tools-slot:todo").fetchSemanticsNode().size.width
        }
        assertTrue("Live Todo insertion retains its ordinary occupied-width animation: $widths",
            widths.any { it > 0 && it < widths.last() })
        assertTrue("The live Todo finishes painting", strongestRed() > 180)
    }

    private fun mount() {
        compose.setContent {
            view = LocalView.current
            MaterialTheme(colorScheme = lightColorScheme(surfaceContainerHigh = Color.Red)) {
                Box(Modifier.width(412.dp).height(240.dp).background(Color.White).testTag("entry-host")) {
                    ConversationToolsRow(owner,
                        if (hasTodo) listOf(ConversationTodoItem("Task")) else emptyList(),
                        emptyList(), {}, {}, contentReady = ready, onMessageInsetChanged = { inset = it })
                }
            }
        }
        compose.waitForIdle()
    }

    private fun frame() {
        Snapshot.sendApplyNotifications()
        compose.mainClock.advanceTimeByFrame()
        compose.waitForIdle()
    }

    private fun strongestRed(): Int {
        val bounds = compose.onNodeWithTag("entry-host").fetchSemanticsNode().boundsInRoot
        val bitmap = Bitmap.createBitmap(ceil(bounds.width).toInt(), ceil(bounds.height).toInt(), Bitmap.Config.ARGB_8888)
        return try {
            compose.runOnUiThread {
                val canvas = Canvas(bitmap)
                canvas.translate(-bounds.left, -bounds.top)
                view.draw(canvas)
            }
            var strongest = 0
            for (y in 0 until bitmap.height) for (x in 0 until bitmap.width) {
                val pixel = Color(bitmap.getPixel(x, y))
                strongest = maxOf(strongest, ((pixel.red - pixel.green) * 255f).toInt())
            }
            strongest
        } finally { bitmap.recycle() }
    }
}
