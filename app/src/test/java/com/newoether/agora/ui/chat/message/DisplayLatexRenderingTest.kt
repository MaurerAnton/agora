package com.newoether.agora.ui.chat.message

import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.toPixelMap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.unit.dp
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** A formula on its own line is drawn, not left as an empty selectable placeholder. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class, qualifiers = "w411dp-h891dp-xxhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class DisplayLatexRenderingTest {
    @get:Rule val compose = createAndroidComposeRule<androidx.activity.ComponentActivity>()

    @Before fun initLatex() {
        ru.noties.jlatexmath.JLatexMathAndroid.init(ApplicationProvider.getApplicationContext())
    }

    @Test fun displayFormulaDrawsInk() {
        val content = "Before\n\n\$\$\nD=\\sqrt{a^2+b^2+c^2}\n\$\$\n\nAfter"
        compose.setContent {
            MaterialTheme {
                val assets = rememberChatMarkdownAssets(textColor = MaterialTheme.colorScheme.onSurface)
                IncrementalStreamingMarkdownContent(
                    content = content,
                    isStreaming = false,
                    renderContext = assets.renderContext,
                    modifier = Modifier.width(360.dp),
                )
            }
        }
        repeat(20) {
            org.robolectric.Shadows.shadowOf(android.os.Looper.getMainLooper()).idle()
            Thread.sleep(50)
            compose.waitForIdle()
        }
        val node = compose.onNode(hasContentDescription("D=\\sqrt{a^2+b^2+c^2}"), useUnmergedTree = true)
        val bounds = node.fetchSemanticsNode().boundsInRoot
        val view = compose.activity.window.decorView
        val bitmap = android.graphics.Bitmap.createBitmap(view.width, view.height, android.graphics.Bitmap.Config.ARGB_8888)
        compose.runOnUiThread { view.draw(android.graphics.Canvas(bitmap)) }
        fun ink(top: Int, bottom: Int, right: Int): Int {
            val bg = bitmap.getPixel(right - 1, top)
            var n = 0
            for (x in 0 until right) for (y in top until bottom) if (bitmap.getPixel(x, y) != bg) n++
            return n
        }
        val formulaInk = ink(bounds.top.toInt(), bounds.bottom.toInt(), bounds.right.toInt())
        assertTrue("multi-line display formula must be drawn, bounds=$bounds", formulaInk > 0)
    }
}
