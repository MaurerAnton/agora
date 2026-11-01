package com.newoether.agora.webui

import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.observeParentConversationId
import com.newoether.agora.data.repository.observeSubagents
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.channels.SendChannel
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.map

/** Lives only in the browser's selected-target scope; never observes unopened owner payloads. */
@OptIn(ExperimentalCoroutinesApi::class)
internal suspend fun ConversationRepository.forwardSubagents(
    conversationId: String?, seq: Long, outbound: SendChannel<WebSyncEvent>,
) {
    if (conversationId == null) {
        outbound.send(WebSyncEvent.Subagents(null, seq, null, emptyList()))
        return
    }
    observeParentConversationId(conversationId).flatMapLatest { parent ->
        observeSubagents(parent ?: conversationId).map {
            WebSyncEvent.Subagents(conversationId, seq, parent, it)
        }
    }.collect { outbound.send(it) }
}
