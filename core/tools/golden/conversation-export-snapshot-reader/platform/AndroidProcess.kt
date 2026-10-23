package android.os

object Process {
    const val THREAD_PRIORITY_BACKGROUND = 10

    fun setThreadPriority(priority: Int) {
        // The stand runs on the test JVM; thread priority is a platform scheduling hint.
    }
}
