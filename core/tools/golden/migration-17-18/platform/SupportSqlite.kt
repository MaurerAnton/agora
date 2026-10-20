// Platform shim for androidx.sqlite.db used by the golden compile. The shapes mirror the androidx
// contracts the migration relies on: overloaded execSQL/query, and query returning an
// android.database.Cursor (the real SupportSQLiteDatabase signature).
package androidx.sqlite.db

interface SupportSQLiteDatabase {
    fun execSQL(sql: String)
    fun execSQL(sql: String, bindArgs: Array<out Any?>)
    fun query(sql: String): android.database.Cursor
    fun query(sql: String, bindArgs: Array<out Any?>?): android.database.Cursor
}
