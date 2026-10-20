// Platform shim for android.database.Cursor used by the golden compile. androidx's
// SupportSQLiteDatabase.query returns this cursor type, so this is the surface Migration16To17.kt
// and Migration17To18.kt observe in production.
package android.database

interface Cursor : java.io.Closeable {
    fun getColumnIndexOrThrow(columnName: String): Int
    fun moveToFirst(): Boolean
    fun moveToNext(): Boolean
    fun getString(columnIndex: Int): String
    fun isNull(columnIndex: Int): Boolean
    fun getLong(columnIndex: Int): Long
}
