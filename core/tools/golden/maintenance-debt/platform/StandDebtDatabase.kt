// Stand-local database holder for the maintenance debt stand.
//
// MaintenanceDebtDao has no ChatDatabase collaborator: every transaction is a @Transaction method
// on the DAO itself. The holder supplies Room's begin/set-success/end stack over the stand
// connection so MiniRoom can wrap those methods exactly like Room's generated adapter.
package com.newoether.agora.data.local

import androidx.room.RoomDatabase
import java.sql.Connection
import stand.room.MiniRoom

class StandDebtDatabase(
    private val connection: Connection,
) : RoomDatabase() {
    private var room: MiniRoom? = null
    private var daoProxy: MaintenanceDebtDao? = null

    fun attach(room: MiniRoom) {
        this.room = room
    }

    fun debtDao(): MaintenanceDebtDao =
        daoProxy ?: room!!.create(MaintenanceDebtDao::class.java).also { daoProxy = it }

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
