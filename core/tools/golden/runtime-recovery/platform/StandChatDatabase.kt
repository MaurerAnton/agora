// Stand-local database holder for the runtime recovery stand.
//
// The recovery entry point is a @Transaction method on ChatDao itself, so the holder supplies
// Room's begin/set-success/end stack over the stand connection and MiniRoom wraps that method
// exactly like Room's generated adapter.
package com.newoether.agora.data.local

import androidx.room.RoomDatabase
import java.sql.Connection
import stand.room.MiniRoom

class StandChatDatabase(
    private val connection: Connection,
) : RoomDatabase() {
    private var room: MiniRoom? = null
    private var daoProxy: ChatDao? = null

    fun attach(room: MiniRoom) {
        this.room = room
    }

    fun chatDao(): ChatDao =
        daoProxy ?: room!!.create(ChatDao::class.java).also { daoProxy = it }

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
