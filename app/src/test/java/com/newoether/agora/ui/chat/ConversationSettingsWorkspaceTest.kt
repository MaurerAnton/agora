package com.newoether.agora.ui.chat

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class ConversationSettingsWorkspaceTest {
    @Test
    fun newChatOwnsSettingsWhilePreviousConversationIdRemainsDuringFade() {
        assertNull(
            conversationSettingsOwnerId(
                isNewChatMode = true,
                currentConversationId = "previous-conversation",
            ),
        )
    }

    @Test
    fun ordinaryConversationOwnsItsSettings() {
        assertEquals(
            "conversation",
            conversationSettingsOwnerId(
                isNewChatMode = false,
                currentConversationId = "conversation",
            ),
        )
    }
}
