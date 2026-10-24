// Stand-local androidx.room surface for compiling data/local/ChatDao.kt and its parents verbatim.
//
// Room is a persistence framework: the annotation processor generates DAO implementations and
// schema DDL at build time. That generated code is the platform boundary of this stand; the
// stand-local MiniRoom (platform/MiniRoom.kt) executes the SQL text the DAO annotations declare.
// To make the declared SQL readable at runtime the annotations use RUNTIME retention here; their
// parameter shapes mirror the Android declarations exactly.
package androidx.room

import kotlin.reflect.KClass

@Target(AnnotationTarget.CLASS)
@Retention(AnnotationRetention.RUNTIME)
annotation class Entity(
    val tableName: String = "",
    val primaryKeys: Array<String> = emptyArray(),
    val foreignKeys: Array<ForeignKey> = [],
    val indices: Array<Index> = [],
)

@Target(AnnotationTarget.CLASS)
@Retention(AnnotationRetention.RUNTIME)
annotation class Dao

@Target(AnnotationTarget.FIELD, AnnotationTarget.PROPERTY, AnnotationTarget.VALUE_PARAMETER)
@Retention(AnnotationRetention.RUNTIME)
annotation class PrimaryKey(val autoGenerate: Boolean = false)

@Retention(AnnotationRetention.RUNTIME)
annotation class ForeignKey(
    val entity: KClass<*>,
    val parentColumns: Array<String>,
    val childColumns: Array<String>,
    val onDelete: Int = NO_ACTION,
) {
    companion object {
        const val NO_ACTION = 1
        const val RESTRICT = 2
        const val SET_NULL = 3
        const val SET_DEFAULT = 4
        const val CASCADE = 5
    }
}

@Retention(AnnotationRetention.RUNTIME)
annotation class Index(
    val value: Array<String>,
    val unique: Boolean = false,
)

@Target(
    AnnotationTarget.FIELD,
    AnnotationTarget.PROPERTY,
    AnnotationTarget.VALUE_PARAMETER,
    AnnotationTarget.FUNCTION,
    AnnotationTarget.CLASS,
)
@Retention(AnnotationRetention.RUNTIME)
annotation class ColumnInfo(val name: String = "", val defaultValue: String = "")

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class TypeConverter

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class Query(val value: String)

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class Insert(val onConflict: Int = OnConflictStrategy.ABORT)

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class Upsert

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class Update(val entity: KClass<*> = Any::class)

@Target(AnnotationTarget.FUNCTION)
@Retention(AnnotationRetention.RUNTIME)
annotation class Transaction

annotation class OnConflictStrategy {
    companion object {
        const val ABORT = 0
        const val REPLACE = 1
        const val IGNORE = 2
        const val FAIL = 3
        const val ROLLBACK = 4
    }
}
