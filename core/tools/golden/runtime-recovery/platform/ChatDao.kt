// Stand-local ChatDao surface for compiling data/local/ChatDaoRuntimeRecovery.kt verbatim.
//
// The recovery extension calls nine ChatDao methods. ChatDao.kt itself is a 795-line interface
// with hundreds of queries, so the stand declares exactly the methods the recovery body touches,
// each with the app's own query text (copied from ChatDao.kt and ChatContextCompactDao.kt —
// the SQL is transcribed here, never rewritten). MiniRoom executes them on the stand schema.
package com.newoether.agora.data.local

import androidx.room.Dao
import androidx.room.Query
import androidx.room.Transaction
import androidx.room.Update
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus

@Dao
interface ChatDao {
    /** ChatContextCompactDao.getConversation */
    @Query("SELECT * FROM conversations WHERE id = :conversationId")
    suspend fun getConversation(conversationId: String): ChatEntity?

    /** ChatContextCompactDao.getLiveRun */
    @Query("SELECT * FROM runs WHERE conversationId = :conversationId AND activeSlot = 1 LIMIT 1")
    suspend fun getLiveRun(conversationId: String): RunEntity?

    /** ChatContextCompactDao.getMessagesForRuns */
    @Query("SELECT * FROM messages WHERE runId IN (:runIds) ORDER BY runSequence, timestamp, id")
    suspend fun getMessagesForRuns(runIds: List<String>): List<MessageEntity>

    /** ChatDao.updateMessageCheckpoint — UPDATE by primary key over the checkpoint projection. */
    @Update(entity = MessageEntity::class)
    suspend fun updateMessageCheckpoint(checkpoint: MessageStreamCheckpoint): Int

    /** ChatDao.terminalizeLiveRun */
    @Query(
        """
        UPDATE runs
        SET status = :status, activeSlot = NULL, lastCheckpointAt = :at, endedAt = :at,
            endReason = :reason
        WHERE id = :runId AND activeSlot = 1
        """,
    )
    suspend fun terminalizeLiveRun(
        runId: String,
        status: RunStatus,
        reason: RunEndReason,
        at: Long,
    ): Int

    /** ChatDao.compareAndSetRunBranchSelections — derived metadata, no dataChangedAt bump. */
    @Query(
        """
        UPDATE conversations
        SET selectedRunBranchesJson = :replacement
        WHERE id = :conversationId AND selectedRunBranchesJson = :expected
        """,
    )
    suspend fun compareAndSetRunBranchSelections(
        conversationId: String,
        expected: String,
        replacement: String,
    ): Int

    /** ChatDao.stopStuckMessagesForConversation */
    @Query(
        """
        UPDATE messages
        SET status = 'STOPPED'
        WHERE conversationId = :conversationId
          AND status IN ('SENDING', 'THINKING', 'TOOL_CALLING', 'TRANSCRIBING')
        """,
    )
    suspend fun stopStuckMessagesForConversation(conversationId: String): Int

    /** ChatDao.touchConversationData */
    @Query(
        "UPDATE conversations SET dataChangedAt = MAX(dataChangedAt + 1, :at) " +
            "WHERE id = :conversationId",
    )
    suspend fun touchConversationData(conversationId: String, at: Long): Int

    /** ChatDao.getRunsForConversationSnapshot */
    @Query("SELECT * FROM runs WHERE conversationId = :conversationId ORDER BY startedAt, id")
    suspend fun getRunsForConversationSnapshot(conversationId: String): List<RunEntity>

    /**
     * Recovers only [conversationId]. The caller must own or have proven the absence of this
     * conversation's process execution lease, so a live Run here is necessarily process-orphaned.
     */
    @Transaction
    suspend fun recoverConversationRuntime(
        conversationId: String,
        at: Long,
    ): Int = executeRuntimeRecovery(conversationId, at)
}
