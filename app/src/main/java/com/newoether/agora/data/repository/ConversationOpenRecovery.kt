package com.newoether.agora.data.repository

import com.newoether.agora.automation.ConversationExecutionCoordinator
import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Shared explicit-open admission; all durable repair stays in ordinary Run recovery. */
internal suspend fun ConversationRepository.recoverOpenedConversationFamily(
    conversationId: String,
    executionCoordinator: ConversationExecutionCoordinator,
) {
    suspend fun recoverOwner(id: String, expectedFamilyId: String? = null) {
        currentCoroutineContext().ensureActive()
        executionCoordinator.tryWithConversationLock(id) {
            // Delegation owns its Job across Compact/ordinary Run handoffs, including idle gaps.
            if (subagentJobs.value[id] != null) return@tryWithConversationLock
            // Membership checks, ordinary recovery and status-only repair are one transaction.
            withProviderContextSnapshot {
                if (expectedFamilyId != null &&
                    (chatDao.getSubagentFamilyRootId(conversationId) != expectedFamilyId ||
                        chatDao.getSubagentFamilyRootId(id) != expectedFamilyId)
                ) return@withProviderContextSnapshot
                val interruptedRunId = chatDao.getLiveRun(id)?.id
                recoverConversationRuntime(id)
                val recoveredInterruption = interruptedRunId?.let { runId ->
                    chatDao.getRun(runId)?.let { run ->
                        run.conversationId == id && run.status.isTerminal &&
                            run.endReason == com.newoether.agora.model.RunEndReason.PROCESS_RECOVERED
                    }
                } == true
                val summary = chatDao.getValidatedSubagentStatus(id)
                    ?: return@withProviderContextSnapshot
                val status = if (recoveredInterruption) "failed" else summary.status
                if (status !in setOf("completed", "failed", "stopped")) {
                    return@withProviderContextSnapshot
                }
                val cachedStatus = chatDao.getSubagentCachedStatus(id)
                if (cachedStatus != status) {
                    chatDao.compareAndSetSubagentStatus(
                        id, summary.parentConversationId, cachedStatus, status,
                        System.currentTimeMillis(),
                    )
                }
            }
        }
    }

    // The visible owner has priority over every related background validation.
    recoverOwner(conversationId)
    val familyId = chatDao.getSubagentFamilyRootId(conversationId) ?: return
    val children = chatDao.getSubagentIds(familyId)
    // A child may widen admission only through its durable indexed parent membership.
    if (familyId != conversationId && conversationId !in children) return
    // One child transaction at a time; never retain child payloads or create runtime sessions.
    for (childId in children) {
        if (childId == conversationId || childId == familyId) continue
        try {
            recoverOwner(childId, familyId)
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (error: Exception) {
            DebugLog.e("ConversationRecovery", "Failed to validate child $childId", error)
        }
    }
}
