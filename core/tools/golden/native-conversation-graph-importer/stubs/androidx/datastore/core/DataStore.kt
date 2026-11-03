// Fake androidx.datastore surface: the stand compiles ImageQualitySettings.kt (data class + store)
// without Android; the store body is never exercised by the archive.
package androidx.datastore.core

import kotlinx.coroutines.flow.Flow

interface DataStore<T> {
    val data: Flow<T>
}

/** androidx.datastore.core.DataMigration (schema migrations; the stand never runs them). */
interface DataMigration<T> {
    suspend fun shouldMigrate(currentData: T): Boolean
    suspend fun migrate(currentData: T): T
    suspend fun cleanUp() {}
}
