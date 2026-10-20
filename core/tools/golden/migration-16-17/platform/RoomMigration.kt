// Platform shims for the golden compile: androidx.room.migration.Migration and the
// androidx.sqlite.db database/cursor surface used by Migration16To17.kt. These are the only
// substituted platform types; the migration itself compiles verbatim from the app tree.
package androidx.room.migration

abstract class Migration(val startVersion: Int, val endVersion: Int) {
    abstract fun migrate(db: androidx.sqlite.db.SupportSQLiteDatabase)
}
