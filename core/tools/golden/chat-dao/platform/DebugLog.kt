// Stand-local DebugLog for compiling the recovery body verbatim.
//
// DebugLog.recoveryStage is release diagnostics: it times each recovery stage into android.util.Log
// (and optionally an upload sink). The stages are the body's own timing instrumentation — they
// carry no decision the recovery makes — so the stand keeps the exact call shape and drops the
// Android sink. This is a documented platform boundary of this stand.
package com.newoether.agora.util

object DebugLog {
    internal fun recoveryStage(
        conversationId: String,
        stage: String,
        elapsedMs: Long,
        rows: Int? = null,
    ) {
        // no-op: the Android logging sink is a platform boundary
    }
}
