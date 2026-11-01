package com.newoether.agora.data.repository

import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.util.Constants
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

private val referenceJson = Json { ignoreUnknownKeys = true }

internal fun MessageEntity.toolRoundReferenceIds(): List<String> = toolCallJson?.let { raw ->
    referenceJson.decodeFromString<List<MessageSegment>>(raw).mapNotNull { it.toolRoundId }.distinct()
}.orEmpty()

internal suspend fun ConversationRepository.loadReferencedToolRounds(
    entity: MessageEntity,
): List<MessageEntity> {
    // Legacy malformed payloads keep their existing projection behavior.
    val ids = runCatching { entity.toolRoundReferenceIds() }.getOrDefault(emptyList())
    return ids.chunked(500).flatMap { getMessagesByIds(it) }
}

internal fun MessageEntity.resolveToolRoundReferences(
    segments: List<MessageSegment>?,
    rounds: List<MessageEntity>,
): List<MessageSegment>? {
    val referencedIds = segments.orEmpty().mapNotNull { it.toolRoundId }.toSet()
    if (referencedIds.isEmpty()) return segments
    val toolsByRound = rounds.filter { it.id in referencedIds }.associate { round ->
        check(round.conversationId == conversationId && runId.isNotBlank() && round.runId == runId) {
            "Tool round reference has a different owner"
        }
        check(round.id.startsWith(Constants.TOOL_MSG_PREFIX) && round.status == MessageStatus.SUCCESS) {
            "Tool round reference is not committed protocol data"
        }
        round.id to referenceJson.decodeFromString<List<MessageSegment>>(checkNotNull(round.toolCallJson))
    }
    return segments?.map { segment ->
        val roundId = segment.toolRoundId ?: return@map segment
        val body = checkNotNull(toolsByRound[roundId]?.singleOrNull {
            it.type == "tool" && it.toolCallId == segment.toolCallId
        }) { "Committed tool body is missing or ambiguous" }
        body.copy(
            toolRoundId = roundId,
            responseOutputItems = emptyList(),
            responseOutputItemProvider = null,
        )
    }
}

internal fun remapToolRoundReferences(raw: String?, messageIds: Map<String, String>): String? {
    if (raw == null) return null
    val segments = runCatching {
        referenceJson.decodeFromString<List<MessageSegment>>(raw)
    }.getOrNull() ?: return raw
    if (segments.none { it.toolRoundId != null }) return raw
    return referenceJson.encodeToString(segments.map { segment ->
        segment.toolRoundId?.let { roundId ->
            segment.copy(toolRoundId = checkNotNull(messageIds[roundId]) {
                "Fork is missing a referenced tool round"
            })
        } ?: segment
    })
}
