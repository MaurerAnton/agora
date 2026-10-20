// Platform shim for android.database.Cursor used by the golden compile.
package android.database

interface Cursor : java.io.Closeable {
    fun getColumnIndexOrThrow(columnName: String): Int
    fun moveToFirst(): Boolean
    fun moveToNext(): Boolean
    fun getString(columnIndex: Int): String
    fun isNull(columnIndex: Int): Boolean
    fun getLong(columnIndex: Int): Long
    fun getInt(columnIndex: Int): Int
}
