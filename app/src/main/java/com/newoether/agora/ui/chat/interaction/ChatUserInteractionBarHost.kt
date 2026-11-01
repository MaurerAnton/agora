package com.newoether.agora.ui.chat.interaction

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.newoether.agora.service.AppForegroundTracker
import com.newoether.agora.viewmodel.ChatViewModel

/** Request drafts, fold and page state stay outside the shared scrolling presentation. */
@Composable
internal fun ChatUserInteractionBarHost(
    viewModel: ChatViewModel,
    conversationId: String?,
    autoWrapCodeBlocks: Boolean,
    content: @Composable (Boolean, Any?, Boolean, Float, @Composable (
        Modifier, (Float) -> Unit, (Float) -> Unit, Boolean, Boolean, (String?, Boolean) -> Unit,
    ) -> Unit) -> Unit,
) {
    val questions by viewModel.askUser.requests.collectAsState()
    val shellCommand by viewModel.shellConfirmation.pendingShellCommand.collectAsState()
    val interactions = remember(questions, shellCommand, conversationId) {
        userInteractions(conversationId, questions, shellCommand)
    }
    DisposableEffect(conversationId) {
        AppForegroundTracker.setPresentedConversation(conversationId)
        onDispose { AppForegroundTracker.setPresentedConversation(null) }
    }
    var minimizedIn by rememberSaveable(saver = MinimizedSaver) { mutableStateOf(emptySet<String>()) }
    val minimizedKey = conversationId.orEmpty()
    val drafts = rememberSaveable(saver = QuestionDrafts.Saver) { QuestionDrafts() }
    drafts.retainOnly(questions)
    var pageIn by rememberSaveable(saver = PageSaver) { mutableStateOf(emptyMap<String, String>()) }
    var observedOwner by remember { mutableStateOf(conversationId) }
    var seenRequests by remember { mutableStateOf(emptySet<Pair<String?, String>>()) }
    var revealSerial by remember { mutableLongStateOf(0L) }
    var revealEvent by remember { mutableStateOf<Pair<String, Long>?>(null) }
    val visibleRequests = buildSet {
        interactions.forEach { interaction ->
            when (interaction) {
                is UserInteraction.Question -> interaction.requests.forEach { request ->
                    add(request.conversationId to "question:${request.id}")
                }
                is UserInteraction.ShellCommand -> {
                    add(interaction.pending.conversationId to interaction.key)
                }
            }
        }
    }
    SideEffect {
        if (observedOwner != conversationId) {
            // Reopening pending requests is navigation, not a new arrival.
            observedOwner = conversationId
            revealEvent = null
        } else if (visibleRequests.any { it !in seenRequests }) {
            revealSerial++
            revealEvent = minimizedKey to revealSerial
        }
        seenRequests = visibleRequests
    }
    var expansionProgress by remember { mutableFloatStateOf(0f) }
    val interactionRevealKey = revealEvent.takeIf {
        observedOwner == conversationId && it?.first == minimizedKey
    }
    content(interactions.isNotEmpty(), interactionRevealKey,
        interactions.isNotEmpty() && minimizedKey !in minimizedIn, expansionProgress) {
            modifier, onHeightChanged, onMessageInsetChanged, appearanceReady, animateEntry, onTransitionChanged ->
        UserInteractionBar(
            conversationId = minimizedKey,
            interactions = interactions,
            minimizedIn = minimizedIn,
            onMinimizedChange = { owner, folded ->
                minimizedIn = if (folded) minimizedIn + owner else minimizedIn - owner
                if (!folded) {
                    revealSerial++
                    revealEvent = owner to revealSerial
                }
            },
            autoWrapCodeBlocks = autoWrapCodeBlocks,
            drafts = drafts,
            pageIn = pageIn,
            onPageChange = { owner, key -> pageIn = pageIn + (owner to key) },
            onCardGone = { owner ->
                if (owner == minimizedKey && interactions.isEmpty()) {
                    minimizedIn = minimizedIn - owner
                    pageIn = pageIn - owner
                }
            },
            onSubmitQuestions = { answers -> viewModel.askUser.submitAll(answers) },
            onSkipQuestion = { id -> viewModel.askUser.dismiss(id) },
            onShellDecision = { id, allow, alwaysAllowServer ->
                viewModel.shellConfirmation.resolve(id, allow, alwaysAllowServer)
            },
            onHeightChanged = onHeightChanged,
            onMessageInsetChanged = onMessageInsetChanged,
            modifier = modifier,
            inSharedRow = true,
            appearanceReady = appearanceReady,
            animateEntry = animateEntry,
            onTransitionChanged = onTransitionChanged,
            onExpansionChanged = { expansionProgress = it },
        )
    }
}

private val PageSaver = listSaver<androidx.compose.runtime.MutableState<Map<String, String>>, String>(
    save = { state -> state.value.flatMap { (owner, key) -> listOf(owner, key) } },
    restore = { saved -> mutableStateOf(saved.chunked(2).associate { (owner, key) -> owner to key }) },
)

private val MinimizedSaver = listSaver<androidx.compose.runtime.MutableState<Set<String>>, String>(
    save = { it.value.toList() },
    restore = { mutableStateOf(it.toSet()) },
)
