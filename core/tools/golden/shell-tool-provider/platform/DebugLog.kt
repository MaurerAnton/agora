package com.newoether.agora.util

/**
 * Stand-local no-op double for util/DebugLog.kt (the android.util.Log wrapper).
 * ShellClient.kt only calls e(tag, msg) and w(tag, msg) to record lastError; the stand keeps
 * the calls silent and adds nothing to the trace (the pinned state is ShellClient.lastError).
 * The android.util.Log invocations of the real object are stubbed out because the stand has no
 * Android SDK; the `active` gate makes them dead code in the real object anyway.
 */
object DebugLog {
    private const val active = false

    fun e(tag: String, msg: String) {
        if (active) { /* android.util.Log.e(tag, msg) */ }
    }

    fun e(tag: String, msg: String, tr: Throwable) {
        if (active) { /* android.util.Log.e(tag, msg, tr) */ }
    }

    fun w(tag: String, msg: String) {
        if (active) { /* android.util.Log.w(tag, msg) */ }
    }

    fun w(tag: String, msg: String, tr: Throwable) {
        if (active) { /* android.util.Log.w(tag, msg, tr) */ }
    }

    fun d(tag: String, msg: String) {
        if (active) { /* android.util.Log.d(tag, msg) */ }
    }
}
