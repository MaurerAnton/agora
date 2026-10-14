package com.newoether.agora.viewmodel

import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

/** Publishes one accepted Send without turning presentation callback failure into Send failure. */
internal class SendAcceptanceNotifier(
    private val onAcceptedEvent: ((conversationId: String, messageId: String) -> Unit)?,
) {
    suspend fun notify(
        acceptance: SendAcceptance,
        onAccepted: suspend (SendAcceptance) -> Unit,
        publishEvent: Boolean = true,
    ) {
        // Draft settlement is authoritative: callers must observe a failure instead of mistaking a
        // presentation callback catch for a successful acknowledgement.
        withContext(NonCancellable) { onAccepted(acceptance) }
        if (publishEvent) publish(acceptance)
    }

    fun publish(acceptance: SendAcceptance) {
        try {
            onAcceptedEvent?.invoke(acceptance.conversationId, acceptance.messageId)
        } catch (error: Exception) {
            DebugLog.e(
                "SendAcceptanceNotifier",
                "Failed to publish accepted Send ${acceptance.messageId}",
                error,
            )
        }
    }
}
