package com.newoether.agora.viewmodel

import com.newoether.agora.model.SubagentSummary
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onStart

/** One selected family's narrow projection, never a cache of other conversation owners. */
internal data class SubagentFamilySnapshot(
    val familyId: String? = null,
    val agents: List<SubagentSummary> = emptyList(),
) {
    /** Retention is safe only for the parent or a child proven by this snapshot. */
    fun forSelection(conversationId: String?): SubagentFamilySnapshot? {
        if (conversationId == null || familyId == null) return null
        return takeIf {
            conversationId == familyId || agents.any {
                it.id == conversationId && it.parentConversationId == familyId
            }
        }
    }
}

@OptIn(ExperimentalCoroutinesApi::class)
internal fun observeSelectedSubagentFamily(
    selectedConversationId: Flow<String?>,
    observeParent: (String) -> Flow<String?>,
    observeSubagents: (String) -> Flow<List<SubagentSummary>>,
): Flow<SubagentFamilySnapshot> = selectedConversationId
    .distinctUntilChanged()
    .flatMapLatest { id ->
        if (id == null) flowOf(null)
        else observeParent(id).map { parent -> parent ?: id }
    }
    // Selection within a family must not restart its Room observer or emit an empty list.
    .distinctUntilChanged()
    .flatMapLatest { familyId ->
        if (familyId == null) flowOf(SubagentFamilySnapshot())
        else observeSubagents(familyId).map { SubagentFamilySnapshot(familyId, it) }
            // An unrelated family query has not confirmed even an empty list yet.
            .onStart { emit(SubagentFamilySnapshot()) }
    }
    .distinctUntilChanged()
