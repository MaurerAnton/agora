// Stand-local util/DebugLog: the import orchestration logs failures and recovery stages; the
// stand keeps the calls side-effect free, exactly like the release logger's no-op sink.
package com.newoether.agora.util

object DebugLog {
    fun e(tag: String, message: String, error: Throwable? = null) = Unit

    fun w(tag: String, message: String, error: Throwable? = null) = Unit

    internal fun recoveryStage(
        conversationId: String,
        stage: String,
        elapsedMs: Long,
        rows: Int? = null,
    ) = Unit
}
