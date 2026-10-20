// Stand-local Room annotations for compiling data/local/RunEntity.kt verbatim.
//
// Room is a persistence framework: these annotations have no runtime behaviour that
// NativeRunArchivePolicy can observe. Their parameter shape mirrors the Android declarations so
// the real RunEntity catalogue entry compiles unchanged.
package androidx.room

import kotlin.reflect.KClass

@Target(AnnotationTarget.CLASS)
@Retention(AnnotationRetention.BINARY)
annotation class Entity(
    val tableName: String = "",
    val foreignKeys: Array<ForeignKey> = [],
    val indices: Array<Index> = [],
)

@Target(AnnotationTarget.FIELD, AnnotationTarget.PROPERTY)
@Retention(AnnotationRetention.BINARY)
annotation class PrimaryKey(val autoGenerate: Boolean = false)

@Retention(AnnotationRetention.BINARY)
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

@Retention(AnnotationRetention.BINARY)
annotation class Index(
    val value: Array<String>,
    val unique: Boolean = false,
)
