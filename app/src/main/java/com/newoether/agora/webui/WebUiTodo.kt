package com.newoether.agora.webui

import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.observeConversationTodo
import kotlinx.coroutines.channels.SendChannel
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.collect

/** One selected-target scope owns these independent projection collectors. */
internal suspend fun ConversationRepository.forwardTargetTools(
    session: WebUiChatSession, target: WebUiChatSession.OpenTarget,
    connectionId: String, outbound: SendChannel<WebSyncEvent>,
) = kotlinx.coroutines.coroutineScope {
    launch { forwardSubagents(target.conversationId, target.browserSeq, outbound) }
    launch { forwardTodo(target.conversationId, target.browserSeq, outbound) }
    launch { forwardBranchReplacement(session, target, connectionId, outbound) }
}

/** Runs only inside the existing selected-target collectLatest scope. */
internal suspend fun ConversationRepository.forwardTodo(
    conversationId: String?, seq: Long, outbound: SendChannel<WebSyncEvent>,
) {
    if (conversationId == null) {
        outbound.send(WebSyncEvent.Todo(null, seq, emptyList()))
    } else {
        observeConversationTodo(conversationId).collect {
            outbound.send(WebSyncEvent.Todo(conversationId, seq, it))
        }
    }
}
