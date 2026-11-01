package com.newoether.agora.viewmodel

import com.newoether.agora.data.SettingsManager
import com.newoether.agora.model.ChatConversation
import com.newoether.agora.service.AppForegroundTracker
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// Runtime-facing wiring for [ChatViewModel], split out of ChatViewModel.kt to stay inside the
// repository's 800-physical-line source budget (development/kotlin-source-size-policy.md).
// Nothing here owns behaviour: every member below was moved verbatim, and every dependency it
// reads from the ViewModel is injected as a supplier so the adapter can still be built at the
// same point of construction while resolving members declared further down the initialiser.

/**
 * This phone UI as a [ChatClient] of the process runtime; attached in init, detached in onCleared.
 */
internal class PhoneChatClient(
    private val scope: CoroutineScope,
    private val selection: () -> ConversationSelectionController,
    private val ui: () -> ConversationUiStateAssembler,
    private val workspaces: ConversationWorkspaceStore,
    private val transitions: () -> BranchReplacementTransitionCoordinator,
    private val scrollRequests: ScrollRequestCoordinator,
    private val sendAcceptedFeedback: () -> ((conversationId: String, messageId: String) -> Unit)?,
    private val submission: () -> ConversationComposerSubmissionController,
    private val shareText: () -> MutableSharedFlow<String>,
    private val firstMessageCommitted: () -> MutableSharedFlow<String>,
    private val emitSnackbar: (String) -> Unit,
) : ChatClient {
    override val openConversationId: String? get() = selection().currentConversationId.value
    override val renderStore: ConversationRenderStore get() = ui().renderStore
    override fun isConversationVisible(conversationId: String): Boolean =
        AppForegroundTracker.isInForeground &&
            AppForegroundTracker.isChatPresented &&
            selection().currentConversationId.value == conversationId
    override val branchTransitions: BranchReplacementTransitionCoordinator
        get() = transitions()
    override suspend fun awaitProjectedPath(conversationId: String, messageId: String) {
        combine(ui().messages, selection().currentConversationId) { path, openConversationId ->
            openConversationId != conversationId || path.any { it.id == messageId }
        }.first { projectedOrClosed -> projectedOrClosed }
    }
    override fun requestScrollToBottomAfter(
        conversationId: String,
        messageId: String,
        attachedOnly: Boolean,
    ) = scrollRequests.requestAbsoluteBottomAfter(conversationId, messageId, attachedOnly)
    override fun onSendAccepted(conversationId: String, messageId: String) {
        // Feedback belongs to the conversation on screen. A send from the new-chat page
        // qualifies because that page becomes this very conversation, but its id is only
        // published after acceptance, so it is matched via isNewChatMode rather than by id.
        val currentId = openConversationId
        val targetsOpenConversation = currentId == conversationId ||
            (currentId == null && selection().isNewChatMode.value)
        if (targetsOpenConversation) sendAcceptedFeedback()?.invoke(conversationId, messageId)
    }
    override suspend fun applyCommittedNewConversationState(conversationId: String) =
        workspaces.applyCommittedNewConversationState(conversationId)
    override suspend fun publishAcceptedNewConversation(
        conversationId: String,
        modelId: String,
        entryId: Long,
    ): Boolean = withContext(Dispatchers.Main.immediate) {
        selection().publishAcceptedConversationIfOriginStillOpen(
            conversationId,
            modelId,
            entryId,
        )
    }.also { selected ->
        if (selected) {
            // The send's own bottom scroll handles the first message; skip the open scroll.
            scrollRequests.suppressNextOpenScroll = true
            firstMessageCommitted().tryEmit(conversationId)
        }
    }
    override suspend fun beginTreeMutation(conversationId: String, scrollToTarget: Boolean) =
        selection().beginTreeMutation(conversationId, scrollToTarget)
    override fun settleTreeMutation(requestId: Long?, targetMessageId: String?) =
        selection().markTreeMutationReady(requestId, targetMessageId)
    override fun failTreeMutation(requestId: Long?) = selection().failTreeMutation(requestId)
    override fun showSnackbar(message: String) = emitSnackbar(message)
    override fun openConversation(conversationId: String) {
        scope.launch { selection().selectConversation(conversationId) }
    }
    override fun showShareText(text: String) {
        shareText().tryEmit(text)
    }
    override fun settleDeletedConversation(conversationId: String, parentConversationId: String?) =
        selection().settleDeletedSelectedConversation(conversationId, parentConversationId)
    override fun isSubmissionFrozen(conversationId: String): Boolean =
        submission().isFrozen(conversationId)
    override fun onGenerationActivityChanged(conversationId: String, active: Boolean) =
        if (active) ui().markActive(conversationId) else ui().markIdle(conversationId)
}

/**
 * Startup jobs deferred until all StateFlow/property backing fields of the ViewModel are
 * initialized — avoids the constructor this-escape where a Dispatchers.IO coroutine accesses a
 * field whose JVM backing field is still null. ChatViewModel keeps a same-named private
 * `startInitJobs()` that delegates here, so behaviour and reachability are unchanged.
 */
internal fun startChatViewModelInitJobs(
    scope: CoroutineScope,
    conversations: StateFlow<List<ChatConversation>?>,
    settingsManager: SettingsManager,
    startProcessServices: () -> Unit,
    proxySettingsSynchronizer: ProxySettingsSynchronizer,
    startupMaintenance: () -> StartupMaintenanceCoordinator,
    localModelCatalogSynchronizer: LocalModelCatalogSynchronizer,
) {
    scope.launch {
        // Apply Tor SOCKS proxy settings
        scope.launch(Dispatchers.IO) {
            combine(
                settingsManager.torEnabled,
                settingsManager.torSocksPort
            ) { enabled, port -> Pair(enabled, port) }
            .collect { (enabled, port) ->
                if (enabled) {
                    com.newoether.agora.api.HttpClient.setProxy(
                        com.newoether.agora.api.HttpClient.ProxyConfig(
                            type = com.newoether.agora.api.HttpClient.ProxyType.SOCKS,
                            host = "127.0.0.1",
                            port = port,
                        )
                    )
                } else {
                    com.newoether.agora.api.HttpClient.setProxy(null)
                }
            }
        }

        conversations.filterNotNull().first()
        startProcessServices()
        proxySettingsSynchronizer.start()
        startupMaintenance().start()
        localModelCatalogSynchronizer.start()
    }
}
