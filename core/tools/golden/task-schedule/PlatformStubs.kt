package androidx.room

/**
 * Stand-local stubs for the two Room markers on TaskEntity.
 *
 * TaskSchedule.kt defines `hasSchedule()` as an extension on TaskEntity, so the file will not
 * compile without that type. TaskEntity itself lives in data/local/ChatEntities.kt alongside the
 * whole entity catalogue, which pulls in androidx.room.* and the model types; it cannot compile
 * host-side. agora_extract task-entity therefore copies just that one declaration out verbatim and
 * points its markers here.
 *
 * Room is a persistence framework, i.e. a platform concern: neither annotation has any runtime
 * behaviour that the schedule logic could observe, which is what makes substituting them a
 * platform-type substitution rather than a transcription of logic under test.
 */

@Target(AnnotationTarget.CLASS)
@Retention(AnnotationRetention.BINARY)
annotation class Entity(val tableName: String = "")

@Target(AnnotationTarget.FIELD, AnnotationTarget.PROPERTY)
@Retention(AnnotationRetention.BINARY)
annotation class PrimaryKey(val autoGenerate: Boolean = false)