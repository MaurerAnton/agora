// Stand-local androidx.room database surface.
//
// Room generates a RoomDatabase subclass and a @Transaction wrapper. The stand keeps the same
// nesting contract — every transaction level has to be marked successful, an unmarked level rolls
// the whole stack back — and issues the matching SQLite transaction statements in the stand
// database; the DAO default methods themselves are compiled verbatim.
package androidx.room

abstract class RoomDatabase {
    private val levels = mutableListOf<Boolean>()
    private var aborted = false

    protected open fun onBegin() {}
    protected open fun onCommit() {}
    protected open fun onRollback() {}

    fun beginTransaction() {
        if (levels.isEmpty()) {
            aborted = false
            onBegin()
        }
        levels.add(false)
    }

    fun setTransactionSuccessful() {
        check(levels.isNotEmpty()) { "RoomDatabase: no transaction in progress" }
        levels[levels.size - 1] = true
    }

    fun endTransaction() {
        check(levels.isNotEmpty()) { "RoomDatabase: no transaction in progress" }
        val successful = levels.removeAt(levels.size - 1)
        if (!successful) aborted = true
        if (levels.isEmpty()) {
            if (aborted) {
                onRollback()
            } else {
                onCommit()
            }
            aborted = false
        }
    }
}

suspend fun <T> RoomDatabase.withTransaction(block: suspend () -> T): T {
    beginTransaction()
    try {
        val result = block()
        setTransactionSuccessful()
        return result
    } finally {
        endTransaction()
    }
}
