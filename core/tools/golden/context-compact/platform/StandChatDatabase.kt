// Stand-local database holder for the context-compact stand.
//
// ChatContextCompactDao is not a `@Dao` itself — ChatDao inherits it — so the stand exposes the
// inherited interface directly through MiniRoom. The two `@Transaction` bodies
// (beginRecompactContextCompact, removeContextCompact) get Room's begin/set-success/end stack
// exactly like Room's generated adapter would give them.
package com.newoether.agora.data.local

import androidx.room.RoomDatabase
import java.sql.Connection
import stand.room.MiniRoom

class StandChatDatabase(
    private val connection: Connection,
) : RoomDatabase() {
    private var room: MiniRoom? = null
    private var compactDaoProxy: ChatContextCompactDao? = null

    fun attach(room: MiniRoom) {
        this.room = room
    }

    fun compactDao(): ChatContextCompactDao =
        compactDaoProxy ?: room!!.create(ChatContextCompactDao::class.java)
            .also { compactDaoProxy = it }

    override fun onBegin() {
        connection.createStatement().use { it.execute("BEGIN") }
    }

    override fun onCommit() {
        connection.createStatement().use { it.execute("COMMIT") }
    }

    override fun onRollback() {
        connection.createStatement().use { it.execute("ROLLBACK") }
    }
}
