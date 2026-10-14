package com.newoether.agora.ui.chat

import androidx.compose.ui.unit.dp
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ChatDrawerContentTest {
    @Test
    fun tasksAndRemoteUse46DpAndKeepJoinedGeometry() {
        val root = generateSequence(File(requireNotNull(System.getProperty("user.dir"))).absoluteFile) {
            it.parentFile
        }.first { File(it, "app/src/main/java").isDirectory }
        val source = File(root,
            "app/src/main/java/com/newoether/agora/ui/chat/ChatDrawerContent.kt").readText()
        val group = source.substringAfter("Column(modifier = Modifier.fillMaxSize()) {")
            .substringBefore("val newChatDisabled")
        assertEquals(2, Regex("""\.height\(46\.dp\)""").findAll(group).count())
        assertFalse(group.contains("height(52.dp)"))
        assertTrue(group.contains("Spacer(modifier = Modifier.height(2.dp))"))
        assertTrue(group.contains("topStart = 24.dp, topEnd = 24.dp"))
        assertTrue(group.contains("bottomStart = 5.dp, bottomEnd = 5.dp"))
        assertTrue(group.contains("topStart = 5.dp, topEnd = 5.dp"))
        assertTrue(group.contains("bottomStart = 24.dp, bottomEnd = 24.dp"))
        assertEquals(2, Regex("""focusManager\.clearFocus\(\)""").findAll(group).count())
        assertEquals(2, Regex("""scope\.launch \{ onRequestClose\(\) \}""").findAll(group).count())
        assertTrue(group.indexOf("onOpenTasks()") < group.indexOf("onOpenRemote()"))
    }
    @Test
    fun generationIndicatorHasPriorityOverUnread() {
        assertEquals(
            DrawerConversationIndicator.GENERATING,
            resolveDrawerConversationIndicator(
                isGenerating = true,
                isSelected = false,
                hasUnreadGeneration = true,
            ),
        )
    }

    @Test
    fun unreadIndicatorIsHiddenForOpenConversation() {
        assertEquals(
            DrawerConversationIndicator.NONE,
            resolveDrawerConversationIndicator(
                isGenerating = false,
                isSelected = true,
                hasUnreadGeneration = true,
            ),
        )
    }

    @Test
    fun backgroundConversationShowsUnreadIndicator() {
        assertEquals(
            DrawerConversationIndicator.UNREAD,
            resolveDrawerConversationIndicator(
                isGenerating = false,
                isSelected = false,
                hasUnreadGeneration = true,
            ),
        )
    }

    @Test
    fun edgeFadeToleranceIsTwoDp() {
        assertEquals(2.dp, DrawerEdgeFadeTolerance)
    }

    @Test
    fun topEdgeAllowsOffsetsThroughTolerance() {
        assertTrue(isDrawerListAtTop(0, 0, 2))
        assertTrue(isDrawerListAtTop(0, 2, 2))
        assertFalse(isDrawerListAtTop(0, 3, 2))
        assertFalse(isDrawerListAtTop(1, 0, 2))
    }

    @Test
    fun bottomEdgeTreatsEmptyListAsReached() {
        assertTrue(
            isDrawerListAtBottom(
                totalItemsCount = 0,
                lastVisibleItemIndex = null,
                lastVisibleItemEndOffsetPx = null,
                viewportEndOffsetPx = 100,
                tolerancePx = 2,
            ),
        )
    }

    @Test
    fun bottomEdgeAllowsFinalItemThroughTolerance() {
        assertTrue(
            isDrawerListAtBottom(
                totalItemsCount = 5,
                lastVisibleItemIndex = 4,
                lastVisibleItemEndOffsetPx = 100,
                viewportEndOffsetPx = 100,
                tolerancePx = 2,
            ),
        )
        assertTrue(
            isDrawerListAtBottom(
                totalItemsCount = 5,
                lastVisibleItemIndex = 4,
                lastVisibleItemEndOffsetPx = 102,
                viewportEndOffsetPx = 100,
                tolerancePx = 2,
            ),
        )
    }

    @Test
    fun bottomEdgeRejectsContentBeyondToleranceOrBeforeFinalItem() {
        assertFalse(
            isDrawerListAtBottom(
                totalItemsCount = 5,
                lastVisibleItemIndex = 4,
                lastVisibleItemEndOffsetPx = 103,
                viewportEndOffsetPx = 100,
                tolerancePx = 2,
            ),
        )
        assertFalse(
            isDrawerListAtBottom(
                totalItemsCount = 5,
                lastVisibleItemIndex = 3,
                lastVisibleItemEndOffsetPx = 100,
                viewportEndOffsetPx = 100,
                tolerancePx = 2,
            ),
        )
        assertFalse(
            isDrawerListAtBottom(
                totalItemsCount = 5,
                lastVisibleItemIndex = 4,
                lastVisibleItemEndOffsetPx = null,
                viewportEndOffsetPx = 100,
                tolerancePx = 2,
            ),
        )
    }
}
