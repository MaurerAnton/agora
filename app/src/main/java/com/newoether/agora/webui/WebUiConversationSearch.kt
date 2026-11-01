package com.newoether.agora.webui

import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.forDisplay
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.ui.chat.findConversationSearchMatches
import com.newoether.agora.ui.chat.scanConversationSearchMatches
import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

private const val TAG = "WebUiConversationSearch"

/** Inputs that require reloading persisted rows for one in-conversation search. */
private data class SearchScanKey(
    val ids: List<String>,
    val command: WebSyncCommand?,
    val providers: List<CustomProviderConfig>,
    val streamingId: String?,
)

/**
 * Serves one opened conversation's in-conversation search for one connection.
 *
 * Only a streaming message inside the searched rows can move matches while a reply streams. Its id
 * is part of the scan key, so persisted rows are scanned once per streaming run; each stream
 * snapshot then re-matches just that one message instead of reloading every page.
 */
internal suspend fun serveConversationSearch(
    conversationId: String,
    seq: Long,
    connectionId: String,
    searchIds: Flow<List<String>>,
    query: Flow<WebSyncCommand?>,
    customProviders: Flow<List<CustomProviderConfig>>,
    payloadRevision: Flow<Long>,
    streamingMessage: Flow<ChatMessage?>,
    loadPage: suspend (List<String>, List<CustomProviderConfig>) -> List<ChatMessage>,
    send: suspend (WebSyncEvent) -> Unit,
) {
    val searchedStreaming = combine(searchIds, streamingMessage) { ids, streaming ->
        streaming?.takeIf { it.id in ids }
    }.distinctUntilChanged()
    combine(
        searchIds, query, customProviders, payloadRevision,
        searchedStreaming.map { it?.id }.distinctUntilChanged(),
    ) { ids, command, providers, _, streamingId ->
        SearchScanKey(ids, command, providers, streamingId)
    }.collectLatest { (ids, command, providers, streamingId) ->
        if (command == null || command.conversationId != conversationId || command.seq != seq) return@collectLatest
        val text = command.text.orEmpty()
        suspend fun publish(searching: Boolean, matches: List<JsonObject> = emptyList(), failed: Boolean = false) {
            send(WebSyncEvent.ConversationSearch(connectionId, conversationId, seq, command.revision, text, searching, matches, failed))
        }
        if (text.isBlank()) { publish(false); return@collectLatest }
        publish(true)
        val persisted = try {
            scanConversationSearchMatches(ids.filterNot { it == streamingId }, text) { page ->
                loadPage(page, providers)
            }.groupBy { it.messageId }
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            DebugLog.e(TAG, "WebUI conversation search failed", error)
            publish(false, failed = true)
            return@collectLatest
        }
        suspend fun publishMatches(streaming: ChatMessage?) {
            val live = streaming?.let { findConversationSearchMatches(listOf(it.forDisplay(providers)), text) }
                .orEmpty()
            val matches = ids.flatMap { messageId ->
                if (messageId == streamingId) live else persisted[messageId].orEmpty()
            }.map { match ->
                buildJsonObject {
                    put("messageId", match.messageId)
                    put("start", match.start)
                    put("endExclusive", match.endExclusive)
                    put("occurrence", match.occurrenceInMessage)
                    put("key", match.key)
                }
            }
            currentCoroutineContext().ensureActive()
            publish(false, matches)
        }
        if (streamingId == null) {
            publishMatches(null)
        } else {
            searchedStreaming.filter { it?.id == streamingId }.collect { publishMatches(it) }
        }
    }
}
