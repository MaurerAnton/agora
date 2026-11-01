package com.newoether.agora.webui

import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class WebUiConversationSearchTest {
    private fun message(id: String, participant: Participant, text: String) = ChatMessage(
        id = id,
        parentId = null,
        text = text,
        participant = participant,
        status = MessageStatus.SUCCESS,
    )

    @Test
    fun streamingFramesRematchOnlyTheStreamingMessage() = runTest {
        val ids = MutableStateFlow(listOf("u1", "m3"))
        val query = MutableStateFlow<WebSyncCommand?>(null)
        val streaming = MutableStateFlow<ChatMessage?>(message("m3", Participant.MODEL, "needle"))
        val loads = mutableListOf<List<String>>()
        val sent = mutableListOf<WebSyncEvent.ConversationSearch>()
        val job = backgroundScope.launch {
            serveConversationSearch(
                conversationId = "a", seq = 1, connectionId = "c",
                searchIds = ids, query = query,
                customProviders = MutableStateFlow(emptyList<CustomProviderConfig>()),
                payloadRevision = MutableStateFlow(0L),
                streamingMessage = streaming,
                loadPage = { page, _ ->
                    loads += page
                    page.map { message(it, Participant.USER, "needle needle") }
                },
                send = { sent += it as WebSyncEvent.ConversationSearch },
            )
        }
        query.value = WebSyncCommand(type = "conversation_search", conversationId = "a", seq = 1, revision = 1, text = "needle")
        runCurrent()
        // The persisted page excludes the streaming row, whose live text is matched instead.
        assertEquals(listOf(listOf("u1")), loads)
        assertEquals(listOf("u1", "u1", "m3"), sent.last().matches.map { it["messageId"].toString().trim('"') })

        streaming.value = message("m3", Participant.MODEL, "needle needle needle")
        runCurrent()
        assertEquals(1, loads.size)
        assertEquals(5, sent.last().matches.size)

        // Streaming ended: the finished row is read back from storage once.
        streaming.value = null
        runCurrent()
        assertEquals(listOf(listOf("u1"), listOf("u1", "m3")), loads)
        job.cancel()
    }
}
