package com.newoether.agora.ui.chat

import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.model.ContextBudget
import com.newoether.agora.util.Constants
import com.newoether.agora.viewmodel.ChatViewModel

internal data class EffectiveConversationControls(
    val settingsOwnerId: String?,
    val codeExecutionEnabled: Boolean,
    val googleSearchEnabled: Boolean,
    val thinkingEnabled: Boolean,
    val thinkingLevel: String,
    val thinkingBudgetEnabled: Boolean,
    val thinkingBudgetTokens: Int,
    val openAiWebSearchAvailable: Boolean,
    val openAiWebSearchEnabled: Boolean,
    val openAiServiceTierState: OpenAiConversationServiceTierState,
    val webSearchAvailable: Boolean,
    val webSearchEnabled: Boolean,
    val shellAvailable: Boolean,
    val shellEnabled: Boolean,
    val showLowContextMode: Boolean,
    val lowContextModeEnabled: Boolean,
    val contextWindow: Int,
)

/** New Chat owns its settings even while the previous conversation id remains under transition. */
internal fun conversationSettingsOwnerId(
    isNewChatMode: Boolean,
    currentConversationId: String?,
): String? = currentConversationId.takeUnless { isNewChatMode }

@Composable
internal fun effectiveConversationControls(
    viewModel: ChatViewModel,
    isNewChatMode: Boolean,
    currentConversationId: String?,
    selectedModel: String,
    customProviders: List<CustomProviderConfig>,
): EffectiveConversationControls {
    val conversationSettings by viewModel.settings.conversationSettings.collectAsState()
    val pendingSettings by viewModel.pendingConversationSettings.collectAsState()
    val settingsOwnerId = conversationSettingsOwnerId(isNewChatMode, currentConversationId)
    val conversationOverride: ConversationSettings? =
        if (isNewChatMode) pendingSettings else settingsOwnerId?.let(conversationSettings::get)
    val globalCodeExecution by viewModel.settings.codeExecutionEnabled.collectAsState()
    val globalGoogleSearch by viewModel.settings.googleSearchEnabled.collectAsState()
    val globalThinkingEnabled by viewModel.settings.thinkingEnabled.collectAsState()
    val globalThinkingLevel by viewModel.settings.thinkingLevel.collectAsState()
    val globalThinkingBudgetEnabled by viewModel.settings.thinkingBudgetEnabled.collectAsState()
    val globalThinkingBudgetTokens by viewModel.settings.thinkingBudgetTokens.collectAsState()
    val globalLocalLowContextModeEnabled by
        viewModel.settings.localLowContextModeEnabled.collectAsState()
    val openAiResponsesApiEnabled by viewModel.settings.openAiResponsesApiEnabled.collectAsState()
    val globalWebSearch by viewModel.settings.webSearchEnabled.collectAsState()
    val globalShell by viewModel.settings.shellEnabled.collectAsState()
    val maxContextWindow by viewModel.settings.maxContextWindow.collectAsState()
    val selectedProviderName = viewModel.getProviderForModel(selectedModel)
    val isEmbeddedLocalModel = selectedProviderName == Constants.PROVIDER_LOCAL

    return EffectiveConversationControls(
        settingsOwnerId = settingsOwnerId,
        codeExecutionEnabled = conversationOverride?.codeExecutionEnabled ?: globalCodeExecution,
        googleSearchEnabled = conversationOverride?.googleSearchEnabled ?: globalGoogleSearch,
        thinkingEnabled = conversationOverride?.thinkingEnabled ?: globalThinkingEnabled,
        thinkingLevel = conversationOverride?.thinkingLevel ?: globalThinkingLevel,
        thinkingBudgetEnabled =
            conversationOverride?.thinkingBudgetEnabled ?: globalThinkingBudgetEnabled,
        thinkingBudgetTokens =
            conversationOverride?.thinkingBudgetTokens ?: globalThinkingBudgetTokens,
        openAiWebSearchAvailable = resolveOpenAiNativeSearchAvailability(
            selectedProviderName,
            openAiResponsesApiEnabled,
            customProviders,
        ),
        openAiWebSearchEnabled = conversationOverride?.openAiWebSearchEnabled ?: true,
        openAiServiceTierState = openAiConversationServiceTierState(
            viewModel,
            conversationOverride,
            selectedProviderName,
            openAiResponsesApiEnabled,
            customProviders,
        ),
        webSearchAvailable = globalWebSearch,
        webSearchEnabled = globalWebSearch && (conversationOverride?.webSearchEnabled ?: true),
        shellAvailable = globalShell,
        shellEnabled = globalShell && (conversationOverride?.shellEnabled ?: true),
        showLowContextMode = isEmbeddedLocalModel,
        lowContextModeEnabled = isEmbeddedLocalModel &&
            (conversationOverride?.lowContextModeEnabled ?: globalLocalLowContextModeEnabled),
        contextWindow = ContextBudget.normalize(conversationOverride?.contextWindow ?: maxContextWindow),
    )
}
