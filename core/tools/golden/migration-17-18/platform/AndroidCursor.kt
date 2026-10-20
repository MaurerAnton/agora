// Platform shim for android.database.Cursor used by the golden compile. androidx's
// SupportSQLiteDatabase.query returns this cursor type; Migration17To18.kt extends it with
// nullableString, so the real type must be present in the compile.
package android.database

interface Cursor : java.io.Closeable {
    fun getColumnIndexOrThrow(columnName: String): Int
    fun moveToFirst(): Boolean
    fun moveToNext(): Boolean
    fun getString(columnIndex: Int): String
    fun isNull(columnIndex: Int): Boolean
    fun getLong(columnIndex: Int): Long
}
