package com.newoether.agora.viewmodel

import com.newoether.agora.model.MessageStatus
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationTerminalNotificationPolicyTest {
    @Test fun compactSuccessNeverNotifiesAcrossHeadlessAndChatVisibility() {
        listOf<Boolean?>(null, false, true).forEach { visible ->
            listOf(false, true).forEach { foreground ->
                listOf(false, true).forEach { pending ->
                    assertFalse(shouldPostGenerationTerminalNotification(
                        messageStatus = MessageStatus.SUCCESS,
                        hasPendingGuidance = pending,
                        isContextCompact = true,
                        appInForeground = foreground,
                        conversationVisible = visible,
                    ))
                }
            }
        }
    }

    @Test fun ordinaryHeadlessCompletionKeepsBackgroundAndGuidancePolicy() {
        fun shouldNotify(status: MessageStatus, foreground: Boolean = false, pending: Boolean = false) =
            shouldPostGenerationTerminalNotification(
                messageStatus = status,
                hasPendingGuidance = pending,
                isContextCompact = false,
                appInForeground = foreground,
                conversationVisible = null,
            )
        assertTrue(shouldNotify(MessageStatus.SUCCESS))
        assertFalse(shouldNotify(MessageStatus.SUCCESS, foreground = true))
        assertFalse(shouldNotify(MessageStatus.SUCCESS, pending = true))
        assertFalse(shouldNotify(MessageStatus.STOPPED))
        assertFalse(shouldNotify(MessageStatus.ERROR))
    }

    @Test fun compactFailureRetainsExistingErrorNotificationPolicy() {
        listOf(false, true).forEach { foreground ->
            assertFalse(shouldPostGenerationTerminalNotification(
                messageStatus = MessageStatus.ERROR,
                hasPendingGuidance = false,
                isContextCompact = true,
                appInForeground = foreground,
                conversationVisible = null,
            ))
            assertTrue(shouldPostGenerationTerminalNotification(
                messageStatus = MessageStatus.ERROR,
                hasPendingGuidance = false,
                isContextCompact = true,
                appInForeground = foreground,
                conversationVisible = false,
            ))
            assertFalse(shouldPostGenerationTerminalNotification(
                messageStatus = MessageStatus.ERROR,
                hasPendingGuidance = false,
                isContextCompact = true,
                appInForeground = foreground,
                conversationVisible = true,
            ))
        }
    }
}
