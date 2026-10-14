package com.newoether.agora.ui.components

import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.ui.chat.message.toRenderableMarkdownText
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Diagnostic only, pending the owner's decision on the matrix defect.
 *
 * Prints what a matrix becomes on the way from the message text to the renderer, so the stage that
 * loses it can be named instead of guessed at.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class LatexMatrixDiagnosticTest {
    @Before
    fun initLatex() {
        ru.noties.jlatexmath.JLatexMathAndroid.init(ApplicationProvider.getApplicationContext())
    }

    private val matrix = "\\begin{pmatrix}\na & b \\\\\nc & d\n\\end{pmatrix}"

    private val messages = linkedMapOf(
        "own paragraph" to "Before\n\n$$\n$matrix\n$$\n\nAfter",
        "alone" to "$$\n$matrix\n$$",
        "one line" to "\$\$$matrix\$\$",
        "bmatrix" to "$$\n\\begin{bmatrix}1 & 0 \\\\ 0 & 1\\end{bmatrix}\n$$",
        "cr rows" to "$$\n\\begin{pmatrix}1 & 2\\cr 3 & 4\\end{pmatrix}\n$$",
        "inline dollar" to "value \$\\begin{pmatrix}1 & 2 \\\\ 3 & 4\\end{pmatrix}\$ here",
        "bare environment" to matrix,
        "no matrix baseline" to "Before\n\n\$\$\nD=\\sqrt{a^2+b^2+c^2}\n\$\$\n\nAfter",
    )

    @Test
    fun reportTheWholeTrip() {
        for ((label, text) in messages) {
            val spans = parseLatexSpans(text, parseInlineDollarMath = true)
            val markdown = text.toRenderableMarkdownText(parseInlineDollarMath = true)
            val recovered = markdown.lines()
                .filter { it.startsWith("![latex](") }
                .mapNotNull { line ->
                    val link = line.substringAfter("(").substringBefore(")")
                    latexImageRequest(link)?.let { "${it.display} ${it.latex.replace("\n", "\\n")}" }
                }
            println(
                "LATEXDIAG | $label | latexSpans=${spans.count { it.isLatex }} | " +
                    "markdown=${markdown.replace("\n", "\\n")}",
            )
            println("LATEXDIAG   recovered=${recovered.joinToString(" ; ").ifEmpty { "NONE" }}")
        }
    }

    /** The renderer itself is proven sound separately; this keeps the focus on the text pipeline. */
    @Test
    fun spanParsingKeepsTheMatrixWhole() {
        val spans = parseLatexSpans("$$\n$matrix\n$$", parseInlineDollarMath = false)
        val formula = spans.single { it.isLatex }
        assertEquals(matrix, formula.content)
        assertEquals(true, formula.display)
    }
}
