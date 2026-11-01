package com.newoether.agora.data.repository

import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.SettingsManager
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach

/** Owns the publication-before-readiness barrier for eagerly collected settings. */
internal class SettingsFlowPublication(private val scope: CoroutineScope) {
    private val initialLoadSignals = mutableListOf<CompletableDeferred<Unit>>()

    fun <T> hot(flow: Flow<T>, initial: T): StateFlow<T> {
        val state = MutableStateFlow(initial)
        publish(flow) { state.value = it }
        return state.asStateFlow()
    }

    fun <T> publish(flow: Flow<T>, publish: (T) -> Unit) {
        val loaded = CompletableDeferred<Unit>()
        initialLoadSignals += loaded
        flow.publishSetting(scope, loaded, publish)
    }

    fun conversationSettings(
        manager: SettingsManager,
        state: ConversationSettingsState,
    ): StateFlow<Map<String, ConversationSettings>> {
        publish(manager.conversationSettings, state::acceptPersisted)
        return state.state
    }

    suspend fun awaitInitialLoad() {
        initialLoadSignals.toList().forEach { it.await() }
    }
}

/** Share the publication-before-readiness rule; the caller owns the target state. */
internal fun <T> Flow<T>.publishSetting(
    scope: CoroutineScope,
    loaded: CompletableDeferred<Unit>,
    publish: (T) -> Unit,
) = onEach { value ->
    // Publish first: an awaiter must never resume while `.value` exposes the eager default.
    publish(value)
    loaded.complete(Unit)
}.catch { error ->
    loaded.completeExceptionally(error)
    throw error
}.launchIn(scope)
