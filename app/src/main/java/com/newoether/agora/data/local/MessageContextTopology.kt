package com.newoether.agora.data.local

import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant

/** Payload-free durable fields used to resolve one canonical selected Provider path. */
data class MessageContextTopology(
    val id: String,
    val conversationId: String,
    val parentId: String?,
    val status: MessageStatus,
    val participant: Participant,
    val timestamp: Long,
    val tokenCount: Int = 0,
    val modelName: String?,
    val runId: String,
    val runSequence: Long,
    val consumedAtPass: Int?,
)

data class ConversationProviderContextState(
    val selectedBranchesJson: String?,
)

data class ProviderContextTopologySnapshot(
    val selectedBranchesJson: String?,
    val messages: List<MessageContextTopology>,
)
