// Platform shim for the golden compile: androidx.room.migration.Migration.
package androidx.room.migration

abstract class Migration(val startVersion: Int, val endVersion: Int) {
    abstract fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase)
}
