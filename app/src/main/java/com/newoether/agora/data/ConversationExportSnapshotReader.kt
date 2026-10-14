package com.newoether.agora.data

import android.content.Context
import android.os.Process
import androidx.room.Transactor
import androidx.room.useReaderConnection
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.local.TaskEntity
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

/**
 * Streams one point-in-time conversation graph from an independent Room connection pool.
 *
 * The dedicated database instance is intentional. A long export must not occupy the process
 * database's transaction executor or a reader connection needed by foreground list, open, search,
 * or generation work. The DEFERRED transaction is read-only, so WAL writers can keep committing
 * checkpoints while every exported table and page observes the same committed snapshot.
 */
internal class ConversationExportSnapshotReader(
    private val context: Context,
) {
    companion object {
        /** Bounds entity/string expansion while exporting databases with large chat histories. */
        private const val MESSAGE_PAGE_SIZE = 64
        private const val SNAPSHOT_THREAD_COUNT = 2
        private val snapshotThreadSequence = AtomicInteger()
    }

    suspend fun readSnapshot(
        onConversation: suspend (ChatEntity) -> Unit,
        onRun: suspend (RunEntity) -> Unit,
        onMessage: suspend (MessageEntity) -> Unit,
        onTask: suspend (TaskEntity) -> Unit,
        onLoop: suspend (LoopEntity) -> Unit,
    ) {
        val snapshotExecutor = Executors.newFixedThreadPool(SNAPSHOT_THREAD_COUNT) { runnable ->
            Thread(
                {
                    Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND)
                    runnable.run()
                },
                "agora-export-db-${snapshotThreadSequence.incrementAndGet()}",
            )
        }
        val snapshotDatabase = ChatDatabase.build(
            context,
            queryExecutor = snapshotExecutor,
            transactionExecutor = snapshotExecutor,
        )
        try {
            val snapshotDao = snapshotDatabase.chatDao()
            snapshotDatabase.useReaderConnection { connection ->
                connection.withTransaction(Transactor.SQLiteTransactionType.DEFERRED) {
                    for (conversation in snapshotDao.getAllConversationsList()) {
                        currentCoroutineContext().ensureActive()
                        onConversation(conversation)
                        for (run in snapshotDao.getRunsForConversationSnapshot(conversation.id)) {
                            currentCoroutineContext().ensureActive()
                            onRun(run)
                        }
                    }

                    var afterMessageId: String? = null
                    while (true) {
                        currentCoroutineContext().ensureActive()
                        val page = snapshotDao.getMessagesPage(afterMessageId, MESSAGE_PAGE_SIZE)
                        if (page.isEmpty()) break
                        for (message in page) {
                            currentCoroutineContext().ensureActive()
                            onMessage(message)
                        }
                        afterMessageId = page.last().id
                        if (page.size < MESSAGE_PAGE_SIZE) break
                    }

                    for (task in snapshotDao.getAllTasksList()) {
                        currentCoroutineContext().ensureActive()
                        onTask(task)
                    }
                    for (loop in snapshotDao.getAllLoopsList()) {
                        currentCoroutineContext().ensureActive()
                        onLoop(loop)
                    }
                }
            }
        } finally {
            snapshotDatabase.close()
            snapshotExecutor.shutdown()
        }
    }
}
