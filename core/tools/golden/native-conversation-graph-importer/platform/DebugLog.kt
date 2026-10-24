package com.newoether.agora.util

object DebugLog {
    fun e(tag: String, message: String, error: Throwable? = null) = Unit

    internal fun recoveryStage(
        conversationId: String,
        stage: String,
        elapsedMs: Long,
        rows: Int? = null,
    ) {
        // no-op: the Android logging sink is a platform boundary
    }
}
