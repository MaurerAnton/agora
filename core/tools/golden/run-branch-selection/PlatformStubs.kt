package androidx.room

/**
 * Stand-local stub for the one Room marker on RunEntity.
 *
 * RunBranchSelectionIntegrity.kt is declared over RunEntity, so the stand needs that type.
 * RunEntity.kt lives next to the rest of the Room catalogue and cannot compile host-side; its
 * `data class` declaration is lifted out verbatim by `agora_extract run-entity`, and the `@PrimaryKey`
 * marker on its `id` points here.
 *
 * Room is a persistence framework, i.e. a platform concern: the marker has no runtime behaviour the
 * selection-integrity policy could observe, which makes this a platform-type substitution rather
 * than a transcription of logic under test.
 */

@Target(AnnotationTarget.FIELD, AnnotationTarget.PROPERTY)
@Retention(AnnotationRetention.BINARY)
annotation class PrimaryKey(val autoGenerate: Boolean = false)
