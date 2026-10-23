// Stand-local Android surface for compiling ConversationExportSnapshotReader.kt verbatim.
//
// The reader only asks the Context for the durable database path and drops the process thread
// priority. Both are platform values with no snapshot decision in them.
package android.content

import java.io.File

class Context(val databaseFile: File) {
    val applicationContext: Context
        get() = this

    fun getDatabasePath(name: String): File = databaseFile
}
