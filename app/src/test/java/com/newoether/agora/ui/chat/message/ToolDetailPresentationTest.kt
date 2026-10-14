package com.newoether.agora.ui.chat.message

import android.app.Application
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.R
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.ToolExecutionStates
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = Application::class)
class ToolDetailPresentationTest {
    @get:Rule val compose = createComposeRule()
    private val resources get() = ApplicationProvider.getApplicationContext<Application>().resources
    private fun segment(name: String, result: String? = null, state: String? = null) =
        MessageSegment(type = "tool", toolName = name, toolResult = result, toolState = state)
    private fun body(segment: MessageSegment) = resources.toolDetailPresentation(segment).body

    @Test fun lifecycleUsesTheExistingResolverWithoutReplayingResults() {
        val active = segment("unknown").copy(toolProgress = "partial")
        assertEquals(ToolDetailBody.Active(resources.toolSummary(active), "partial"), body(active))
        assertTrue(body(segment("unknown")) is ToolDetailBody.Active)
        val failed = segment("mcp_test", "legacy", ToolExecutionStates.FAILED).copy(
            toolResultText = "not visible", toolStructuredResult = "{\"private\":1}",
        )
        assertEquals(ToolDetailBody.Failed(resources.getString(R.string.tool_call_failed), null), body(failed))
        assertEquals(
            ToolDetailBody.Stopped(resources.getString(R.string.tool_execution_stopped)),
            body(segment("web_search", "{}", ToolExecutionStates.STOPPED)),
        )
        val background = segment("unknown", "{\"output\":\"live\"}", ToolExecutionStates.BACKGROUND_RUNNING)
        assertEquals(ToolDetailBody.Active(resources.toolSummary(background), "live"), body(background))
    }

    @Test fun shellKeepsCommandExitCodeAndCompletedOutputPriority() {
        val shell = segment("execute_shell_command", "{\"exit_code\":7,\"output\":\"done\"}").copy(
            toolTarget = "Device", toolProgress = "old",
        )
        val presentation = ToolPresentationResolver.resolve(shell)
        assertEquals(ToolPresentationState.COMPLETED, presentation.state)
        assertEquals(ToolDetailBody.Shell(resources.shellExecutionSummary(presentation), "Device", null, "done"), body(shell))
        assertEquals(
            resources.getString(R.string.tool_no_output),
            (body(segment("wait_for_job").copy(toolProgress = "Connecting to Device")) as ToolDetailBody.Shell).output,
        )
    }

    @Test fun fileListsAndMalformedMatchesKeepEmptyAndOrderedSemantics() {
        assertEquals(ToolDetailBody.Paths(listOf("a", "b")), body(segment("file_glob", "{\"files\":[\"a\",\"b\"]}")))
        assertEquals(ToolDetailBody.Muted(resources.getString(R.string.tool_found_no_files)), body(segment("file_glob", "{}")))
        assertEquals(ToolDetailBody.Muted(resources.getString(R.string.tool_found_no_matches)), body(segment("file_grep", "{\"matches\":[null,1]}")))
        val unknown = resources.getString(R.string.file_path_unknown)
        val grep = body(segment("file_grep", """{"matches":[{"content":"first"},{"path":"$unknown","line":3,"content":"second"},{"path":"","content":"third"}]}""")) as ToolDetailBody.Grep
        assertEquals(2, grep.groups.size)
        assertEquals(listOf("first", "third"), grep.groups[0].matches.map { it.content })
        assertEquals(listOf(ToolDetailMatch(3, "second")), grep.groups[1].matches)
    }

    @Test fun fileTruncationUsesReturnedBytesOrUtf8Content() {
        val file = body(segment("file_read", """{"path":"/tmp/a","lines":1,"content":"é","offset":10,"truncated":true}""")) as ToolDetailBody.FileContent
        assertEquals(resources.getString(R.string.tool_read_file_truncated, 12L), file.truncationText)
        assertEquals(resources.getString(R.string.tool_line_count, 1), file.lineCount)
        val bytes = body(segment("file_read", """{"content":"abc","returned_bytes":7,"offset":10,"truncated":true}""")) as ToolDetailBody.FileContent
        assertEquals(resources.getString(R.string.tool_read_file_truncated, 17L), bytes.truncationText)
    }

    @Test fun searchKeepsDisplayUrlsButOnlySafeDestinationsAreClickable() {
        val search = body(segment("web_search", """{"results":[{"href":"https://example.com","description":"desc"},{"url":"javascript:alert(1)","body":"body"},null]}""")) as ToolDetailBody.Search
        assertEquals("https://example.com", search.results[0].safeUrl)
        assertEquals("desc", search.results[0].snippet)
        assertEquals("javascript:alert(1)", search.results[1].url)
        assertNull(search.results[1].safeUrl)
        assertEquals(resources.getString(R.string.tool_web_result, 3), search.results[2].title)
        assertTrue(body(segment("web_search", "{\"results\":[]}")) is ToolDetailBody.Muted)
    }

    @Test fun mcpAndUnknownJsonKeepOriginalStringsForThePrefixAwareRenderer() {
        val mcp = segment("mcp_test", "legacy").copy(toolResultText = "text", toolStructuredResult = "{\"a\":1}")
        assertEquals(ToolDetailBody.JsonContent(listOf("text", "{\"a\":1}")), body(mcp))
        val raw = segment("unknown", "{\"a\":").copy(toolArgs = "{\"long\":")
        assertEquals(raw.toolArgs, resources.toolDetailPresentation(raw).arguments)
        assertEquals(ToolDetailBody.JsonContent(listOf(raw.toolResult!!)), body(raw))
        assertNull(resources.toolDetailPresentation(raw.copy(toolArgs = "{}")).arguments)
    }

    @Test fun composeRendersTheSharedShellAndFileDetails() {
        val current = mutableStateOf(segment("execute_shell_command", "{\"exit_code\":7,\"output\":\"command output\"}"))
        compose.setContent { MaterialTheme { ToolDetailContent(current.value) { _, _ -> } } }
        compose.onNodeWithText("command output").assertExists()
        compose.runOnIdle { current.value = segment("file_read", "{\"path\":\"/file\",\"content\":\"file content\"}") }
        compose.onNodeWithText("/file").assertExists()
        compose.onNodeWithText("file content").assertExists()
        compose.onNodeWithText("command output").assertDoesNotExist()
    }

    @Test @Config(qualifiers = "zh") fun composeFailureAndStopKeepLocalizedTerminalTextOnly() {
        val current = mutableStateOf(segment("mcp_test", "", ToolExecutionStates.FAILED))
        compose.setContent { MaterialTheme { ToolDetailContent(current.value) { _, _ -> } } }
        compose.onNodeWithText(resources.getString(R.string.tool_call_failed)).assertExists()
        compose.onNodeWithText("hidden result").assertDoesNotExist()
        compose.runOnIdle { current.value = segment("web_search", "{}", ToolExecutionStates.STOPPED) }
        compose.onNodeWithText(resources.getString(R.string.tool_execution_stopped)).assertExists()
    }
}
