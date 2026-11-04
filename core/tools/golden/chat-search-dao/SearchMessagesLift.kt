// Stand-local lift of data/repository/ConversationMessageSearch.kt for the chat-search-dao stand.
// Upstream moved searchMessages from the DAO into the repository layer; the stand compiles
// ChatSearchDao rather than ChatDao, so the entry point takes ChatSearchDao. The LIKE escaping,
// the bounded keyword cursor and the citation-title matching are copied verbatim.
package com.newoether.agora.data.repository

import com.newoether.agora.data.local.ChatSearchDao
import com.newoether.agora.data.local.KeywordSearchRow
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.citationRecords
import com.newoether.agora.model.matchesCitationTitle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.coroutines.yield
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json

internal fun MessageEntity.matchesCitationTitle(query: String): Boolean {
    return citationTitleMatches(toolCallJson, text, query)
}

private fun citationTitleMatches(raw: String?, text: String, query: String): Boolean {
    if (raw == null) return false
    val segments = runCatching { Json.decodeFromString<List<MessageSegment>>(raw) }
        .getOrNull().orEmpty()
    return segments.citationRecords(text).matchesCitationTitle(query)
}
private val citationSearchNewestFirst =
    compareByDescending<MessageEntity>(MessageEntity::timestamp).thenByDescending(MessageEntity::id)

internal suspend fun searchConversationMessages(
    dao: ChatSearchDao,
    query: String,
    limit: Int,
): List<MessageEntity> = withContext(Dispatchers.IO) {
    if (query.isBlank() || limit <= 0) return@withContext emptyList()
    val escapedQuery = escapeLikePattern(query)
    // SQLite LIKE folds only ASCII letters. Other literals can use its faster exact substring scan.
    val literalQuery = query.takeUnless { value -> value.any { it in 'a'..'z' || it in 'A'..'Z' } }
    val ids = boundedKeywordMatches(query, limit) { before, pageSize ->
        if (before == null) dao.getKeywordSearchFirstPage(escapedQuery, pageSize, literalQuery)
        else dao.getKeywordSearchNextPage(escapedQuery, before.timestamp, before.id, pageSize, literalQuery)
    }
    if (ids.isEmpty()) return@withContext emptyList()
    // Visibility may change while pages are scanned. Hydrate only the bounded winners.
    dao.getSearchableMessagesByIds(ids).sortedWith(citationSearchNewestFirst)
}

internal suspend fun boundedKeywordMatches(
    query: String,
    limit: Int,
    pageSize: Int = 128,
    loadPage: suspend (before: KeywordSearchRow?, pageSize: Int) -> List<KeywordSearchRow>,
): List<String> {
    if (query.isBlank() || limit <= 0) return emptyList()
    val boundedPageSize = pageSize.coerceIn(1, 256)
    val ids = mutableListOf<String>()
    var before: KeywordSearchRow? = null
    while (true) {
        currentCoroutineContext().ensureActive()
        val page = loadPage(before, boundedPageSize)
        if (page.isEmpty()) return ids
        for (row in page) {
            currentCoroutineContext().ensureActive()
            val previous = before
            check(previous == null || row.timestamp < previous.timestamp ||
                row.timestamp == previous.timestamp && row.id < previous.id) {
                "Keyword search page did not advance its cursor"
            }
            before = row
            if (row.directMatch || citationTitleMatches(row.toolCallJson, "", query)) {
                ids += row.id
                if (ids.size == limit) return ids
            }
        }
        if (page.size < boundedPageSize) return ids
        // The DAO suspension ends its read; CPU matching never holds a database connection.
        yield()
    }
}

/** Escapes LIKE wildcards so a literal "%"/"_" in the user's query matches itself
 *  instead of matching everything (paired with ESCAPE '\' in the DAO query). */
private fun escapeLikePattern(query: String): String =
    query.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
