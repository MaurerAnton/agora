package com.newoether.agora.ui.chat
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface

import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.Stable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.DefaultSystemPrompt
import com.newoether.agora.data.SystemPromptEntry
import com.newoether.agora.data.replaceCustomProviderIdsForDisplay
import com.newoether.agora.ui.common.AgoraHaptics
import com.newoether.agora.ui.components.DialogWindowEdgeToEdge
import com.newoether.agora.ui.settings.SystemPromptEditorPage
import com.newoether.agora.viewmodel.ChatViewModel
import kotlinx.coroutines.launch

internal enum class ChatDeleteDialogPhase {
    CONFIRM,
    PENDING,
    FAILED,
}

@Stable
internal class ChatAppDialogState internal constructor(
    private val manualCompactVisibleState: MutableState<Boolean>,
) {
    var renameConversationId by mutableStateOf<String?>(null)
        private set
    var renameInitialName by mutableStateOf("")
        private set
    var deleteConversationId by mutableStateOf<String?>(null)
        private set
    var deleteConversationPhase by mutableStateOf(ChatDeleteDialogPhase.CONFIRM)
        private set
    var promptVisible by mutableStateOf(false)
        private set
    var advancedVisible by mutableStateOf(false)
        private set
    val manualCompactVisible: Boolean
        get() = manualCompactVisibleState.value

    fun requestRename(conversationId: String, initialName: String) {
        renameConversationId = conversationId
        renameInitialName = initialName
    }

    fun dismissRename() {
        renameConversationId = null
    }

    fun requestDelete(conversationId: String) {
        if (deleteConversationPhase == ChatDeleteDialogPhase.PENDING) return
        deleteConversationId = conversationId
        deleteConversationPhase = ChatDeleteDialogPhase.CONFIRM
    }

    fun beginDelete(conversationId: String): Boolean {
        if (
            deleteConversationId != conversationId ||
            deleteConversationPhase == ChatDeleteDialogPhase.PENDING
        ) {
            return false
        }
        deleteConversationPhase = ChatDeleteDialogPhase.PENDING
        return true
    }

    fun isDeletePending(conversationId: String): Boolean =
        deleteConversationId == conversationId &&
            deleteConversationPhase == ChatDeleteDialogPhase.PENDING

    fun completeDelete(conversationId: String) {
        if (deleteConversationId != conversationId) return
        deleteConversationId = null
        deleteConversationPhase = ChatDeleteDialogPhase.CONFIRM
    }

    fun failDelete(conversationId: String) {
        if (deleteConversationId == conversationId) {
            deleteConversationPhase = ChatDeleteDialogPhase.FAILED
        }
    }

    fun dismissDelete() {
        if (deleteConversationPhase == ChatDeleteDialogPhase.PENDING) return
        deleteConversationId = null
        deleteConversationPhase = ChatDeleteDialogPhase.CONFIRM
    }

    fun showPrompt() {
        promptVisible = true
    }

    fun dismissPrompt() {
        promptVisible = false
    }

    fun showAdvanced() {
        advancedVisible = true
    }

    fun dismissAdvanced() {
        advancedVisible = false
    }

    fun showManualCompact() {
        manualCompactVisibleState.value = true
    }

    fun dismissManualCompact() {
        manualCompactVisibleState.value = false
    }
}

@Composable
internal fun rememberChatAppDialogState(
    manualCompactVisibleState: MutableState<Boolean>,
): ChatAppDialogState = remember(manualCompactVisibleState) {
    ChatAppDialogState(manualCompactVisibleState)
}

@Composable
internal fun ChatAppDialogHost(
    state: ChatAppDialogState,
    viewModel: ChatViewModel,
    haptics: AgoraHaptics,
    compactModel: String?,
    selectedModel: String,
    compactPrompt: String,
    compactRetainCount: Int,
    enabledModels: Set<String>,
    modelAliases: Map<String, String>,
    modelProviderNames: Map<String, Boolean>,
    customProviders: List<CustomProviderConfig>,
    isCompacting: Boolean,
) {
    var promptDraft by remember { mutableStateOf<SystemPromptEntry?>(null) }
    var pendingCreatedPromptId by remember { mutableStateOf<String?>(null) }
    var savingPromptDraft by remember { mutableStateOf(false) }
    val promptEditorScope = rememberCoroutineScope()
    val systemPrompts by viewModel.settings.systemPrompts.collectAsState()
    val showDocFab by viewModel.settings.showDocumentationFab.collectAsState()
    val currentConversationId by viewModel.currentConversationId.collectAsState()
    val createdPromptId = pendingCreatedPromptId?.takeIf { id ->
        systemPrompts.any { it.id == id }
    }

    state.renameConversationId?.let { id ->
        ChatRenameDialog(
            initialName = state.renameInitialName,
            initialDisplayName = replaceCustomProviderIdsForDisplay(
                state.renameInitialName,
                customProviders,
            ),
            onSave = { newName ->
                viewModel.renameConversation(id, newName)
                state.dismissRename()
            },
            onDismiss = state::dismissRename,
        )
    }

    state.deleteConversationId?.let { id ->
        val deleteConversation = {
            val accepted = viewModel.deleteConversation(id) { deleted ->
                if (deleted) {
                    state.completeDelete(id)
                    haptics.destructiveConfirmed()
                } else {
                    state.failDelete(id)
                }
            }
            if (!accepted) state.failDelete(id)
        }
        ChatDeleteConfirmDialog(
            phase = state.deleteConversationPhase,
            onConfirm = {
                if (state.beginDelete(id)) {
                    promptEditorScope.launch {
                        // Keep the confirmation window blocking until the result arrives.
                        withFrameNanos { }
                        if (!state.isDeletePending(id)) return@launch
                        deleteConversation()
                    }
                }
            },
            onDismiss = state::dismissDelete,
        )
    }

    if (state.promptVisible) {
        ChatSystemPromptDialog(
            viewModel = viewModel,
            createdPromptId = createdPromptId,
            onCreatedPromptConsumed = { pendingCreatedPromptId = null },
            onCreate = { promptDraft = DefaultSystemPrompt.create().copy(title = "") },
            onDismiss = state::dismissPrompt,
        )
    }

    promptDraft?.let { draft ->
        Dialog(
            onDismissRequest = { if (!savingPromptDraft) promptDraft = null },
            properties = DialogProperties(
                usePlatformDefaultWidth = false,
                decorFitsSystemWindows = false,
            ),
        ) {
            DialogWindowEdgeToEdge()
            Surface(
                modifier = Modifier.fillMaxSize(),
                color = MaterialTheme.colorScheme.background,
            ) {
                SystemPromptEditorPage(
                    entry = draft,
                    isNew = true,
                    saveEnabled = !savingPromptDraft,
                    onSave = { title, systemItems, userItems, assistantItems ->
                        if (!savingPromptDraft) {
                            savingPromptDraft = true
                            promptEditorScope.launch {
                                try {
                                    pendingCreatedPromptId = viewModel.settings.addSystemPromptAndAwait(
                                        id = draft.id,
                                        title = title,
                                        systemItems = systemItems,
                                        userItems = userItems,
                                        assistantItems = assistantItems,
                                    )
                                    promptDraft = null
                                } finally {
                                    savingPromptDraft = false
                                }
                            }
                        }
                    },
                    onBack = { if (!savingPromptDraft) promptDraft = null },
                    showDocFab = showDocFab,
                )
            }
        }
    }

    if (state.advancedVisible) {
        ChatAdvancedSettingsDialog(viewModel = viewModel, onDismiss = state::dismissAdvanced)
    }

    if (state.manualCompactVisible) {
        ChatManualCompactDialog(
            initialModel = compactModel ?: selectedModel,
            initialPrompt = compactPrompt,
            initialRetainCount = compactRetainCount,
            enabledModels = enabledModels,
            modelAliases = modelAliases,
            modelProviderNames = modelProviderNames,
            customProviders = customProviders,
            isCompacting = isCompacting,
            onCompact = { model, prompt, retainCount ->
                state.dismissManualCompact()
                viewModel.compactUi.startManual(model, prompt, retainCount)
            },
            onDismiss = state::dismissManualCompact,
        )
    }
}
