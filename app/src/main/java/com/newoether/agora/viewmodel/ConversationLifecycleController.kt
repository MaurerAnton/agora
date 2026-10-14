package com.newoether.agora.viewmodel

import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Coordinates durable conversation metadata changes and deletion cleanup. */
internal class ConversationLifecycleController(
    private val currentConversationId: StateFlow<String?>,
    private val conversations: ConversationRepository,
    private val scope: CoroutineScope,
    private val stopLoop: suspend (String) -> Unit,
    private val tryWithConversationLock: suspend (String, suspend () -> Unit) -> Boolean,
    private val removeRuntime: (String) -> Unit,
    private val stopVisibleGeneration: () -> Unit,
    private val settleDeletedSelectedConversation: (String) -> Unit,
    private val beginSelectedDeleteTransition: suspend (String) -> Long? = { null },
    private val abortSelectedDeleteTransition: (Long?) -> Unit = {},
    private val isDeleteLocked: (String) -> Boolean = { false },
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
    private val mainDispatcher: CoroutineDispatcher = Dispatchers.Main,
) {
    fun rename(conversationId: String, newTitle: String) {
        scope.launch {
            conversations.updateConversationTitle(conversationId, newTitle)
        }
    }

    fun delete(
        conversationId: String,
        expectedMessageIds: Set<String>? = null,
        onResult: (Boolean) -> Unit = {},
    ): Boolean {
        if (isDeleteLocked(conversationId)) return false
        scope.launch(ioDispatcher) {
            var deleted = false
            var selectedAtCommit = false
            val selectedAtDispatch = currentConversationId.value == conversationId
            var transitionRequestId: Long? = null
            try {
                if (selectedAtDispatch) {
                    // This suspends for the overlay fade, so no destructive storage work can begin
                    // until the underlying selected-page loading surface is visible.
                    transitionRequestId = beginSelectedDeleteTransition(conversationId)
                }
                tryWithConversationLock(conversationId) {
                    // Send admission uses this same lock. Only the winner may stop live work.
                    if (isDeleteLocked(conversationId)) return@tryWithConversationLock
                    if (
                        expectedMessageIds != null &&
                        conversations.getMessageTopologySnapshot(conversationId)
                            .mapTo(linkedSetOf()) { it.id } != expectedMessageIds
                    ) return@tryWithConversationLock
                    selectedAtCommit = currentConversationId.value == conversationId
                    if (selectedAtCommit) stopVisibleGeneration()
                    stopLoop(conversationId)
                    conversations.deleteConversation(conversationId)
                    deleted = true
                }
                if (deleted) {
                    removeRuntime(conversationId)
                    if (selectedAtCommit) {
                        withContext(mainDispatcher) {
                            // Hand the already-visible overlay to the New Chat transition.
                            settleDeletedSelectedConversation(conversationId)
                        }
                    }
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                runCatching {
                    DebugLog.e(
                        "ConversationLifecycle",
                        "Failed to delete conversation $conversationId",
                        error,
                    )
                }
            } finally {
                withContext(NonCancellable + mainDispatcher) {
                    if (selectedAtDispatch && (!deleted || !selectedAtCommit)) {
                        abortSelectedDeleteTransition(transitionRequestId)
                    }
                    onResult(deleted)
                }
            }
        }
        return true
    }
}
