package com.newoether.agora.viewmodel

import com.newoether.agora.api.HttpClient
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.diagnostics.DeveloperDiagnostics
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext

/** Complete immutable input for launching one already-bound generation execution. */
internal data class BoundRunGenerationRequest(
    val conversationId: String,
    val modelMessageId: String,
    val startTime: Long,
    val snapshot: GenerationAdmissionSnapshot,
    val uiToken: Long,
    val persistId: Long,
    val runId: String,
    val pass: Int,
    val requestKind: String,
    val transformFinalText: (String, MessageStatus) -> String = { text, _ -> text },
) {
    init {
        require(snapshot.conversationId == conversationId)
        require(snapshot.runId == runId)
    }
}

/**
 * Executes the shared generation tail after the caller has durably created and bound the Run.
 *
 * This component owns no Run state, Job, scope, or continuation decision. Provider/tool outcomes
 * still return through the identified callbacks supplied by the conversation runtime host.
 */
internal data class AutomaticCompactContinuationRequest(
    val generationRequest: BoundRunGenerationRequest,
    val parentMessageId: String,
    val config: AutomaticCompactConfig,
)

internal class BoundRunGenerationLauncher(
    private val conversations: ConversationRepository,
    private val generationManagerProvider: () -> GenerationManager,
    private val automaticCompactNeeded: suspend (
        conversationId: String,
        contextLimit: Int,
        config: AutomaticCompactConfig,
    ) -> Boolean,
    private val terminalSettlement: GenerationTerminalSettlementController,
    private val toUiMessage: (MessageEntity) -> ChatMessage,
    private val isConversationVisible: ((String) -> Boolean)? = null,
    private val onAutomaticCompactContinuation: (
        request: AutomaticCompactContinuationRequest,
        state: ConversationGenerationState,
    ) -> Unit = { _, _ -> },
    /**
     * Fired when a chat answer aborts on CJK text. Return true when a replacement
     * run was launched (the aborted attempt is then swallowed); false rethrows,
     * preserving plain Stop semantics. Defaults to no auto-retry (tasks, compactions).
     */
    private val onCjkAutoRetry: (conversationId: String, modelMessageId: String) -> Boolean =
        { _, _ -> false },
    private val clock: () -> Long = System::currentTimeMillis,
) {
    suspend fun launch(
        request: BoundRunGenerationRequest,
        state: ConversationGenerationState,
    ) {
        val requestTrace = HttpClient.RequestTrace(
            requestId = request.modelMessageId,
            origin = request.requestKind,
            diagnosticContext = DeveloperDiagnostics.newRequestContext(
                requestId = request.modelMessageId,
                conversationId = request.conversationId,
                runId = request.runId,
                pass = request.pass,
                provider = request.snapshot.config.providerName,
                model = request.snapshot.selectedModelId,
                requestKind = request.requestKind,
            ),
        )
        requestTrace.mark(
            "prepare_start",
            "acceptedDelayMs=${(clock() - request.startTime).coerceAtLeast(0L)}",
        )
        try {
            requestTrace.mark("request_config_ready")
            val generationManager = generationManagerProvider()
            // CJK auto-retry only applies to plain chat; titles, transcription, compactions
            // and tasks keep the configured value untouched by forcing the flag off.
            val snapshot = if (request.requestKind == "chat") {
                request.snapshot
            } else {
                request.snapshot.copy(
                    config = request.snapshot.config.copy(autoRegenOnCjk = false),
                )
            }
            val fixedTokenCost = generationManager.resolvedFixedContextTokenCost(
                snapshot.config,
                snapshot.context,
            )
            val automaticCompactConfig = request.snapshot.automaticCompact.copy(
                fixedTokenCost = fixedTokenCost,
                mainModelId = snapshot.config.modelId,
                includeAssistantReasoning = generationManager.includesAssistantReasoning(
                    snapshot.config,
                    snapshot.context,
                ),
            )
            try {
                val result = generationManager.generate(
                    conversationId = request.conversationId,
                    modelMessageId = request.modelMessageId,
                    startTime = request.startTime,
                    modelName = snapshot.selectedModelId,
                    runId = request.runId,
                    pass = request.pass,
                    ownerToken = request.uiToken,
                    config = snapshot.config,
                    ctx = snapshot.context,
                    providerInstances = snapshot.providerInstances,
                    generationJob = currentCoroutineContext()[Job],
                    callbacks = state.callbacksFor(request.uiToken, request.persistId).copy(
                        isConversationVisible = isConversationVisible?.let { visible ->
                            { visible(request.conversationId) }
                        },
                        transformFinalText = request.transformFinalText,
                        onToolRoundPersisted = {
                            if (
                                automaticCompactNeeded(
                                    request.conversationId,
                                    snapshot.config.maxContextWindow,
                                    automaticCompactConfig,
                                )
                            ) {
                                ToolRoundBoundaryDecision.CompleteForFollowUp
                            } else {
                                ToolRoundBoundaryDecision.Continue
                            }
                        },
                    ),
                    streamScope = state.streamScope,
                    requestTrace = requestTrace,
                )
                result.followUpParentMessageId?.let { parentMessageId ->
                    onAutomaticCompactContinuation(
                        AutomaticCompactContinuationRequest(
                            generationRequest = request,
                            parentMessageId = parentMessageId,
                            config = automaticCompactConfig,
                        ),
                        state,
                    )
                }
            } catch (aborted: CjkAutoRetryAbort) {
                if (onCjkAutoRetry(request.conversationId, request.modelMessageId)) return
                throw aborted
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            DebugLog.e(
                "AgoraVM",
                "Generation failed in ${request.requestKind} " +
                    "errorType=${e.javaClass.simpleName}",
            )
            // A pre-stream failure would otherwise strand the SENDING placeholder and overlay.
            runCatching {
                val existing = conversations.getMessage(request.modelMessageId)
                if (existing != null && existing.status == MessageStatus.SENDING) {
                    terminalSettlement.finalizeBoundFailure(
                        conversationId = request.conversationId,
                        runId = request.runId,
                        pass = request.pass,
                        uiToken = request.uiToken,
                        state = state,
                        failedMessage = toUiMessage(existing).copy(
                            text = "Error: ${e.localizedMessage ?: "Failed to build the request."}",
                            status = MessageStatus.ERROR,
                        ),
                        effectId = "request-finalize-${request.runId}-${request.pass}",
                    )
                }
            }
        }
    }
}
