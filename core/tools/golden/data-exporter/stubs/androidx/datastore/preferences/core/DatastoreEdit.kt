// Fake androidx.datastore Preferences surface for the merged ImageQualitySettingsStore: the key
// factories and the Preferences interface live in platform/Preferences.kt; only the edit surface
// is added here.
package androidx.datastore.preferences.core

import androidx.datastore.core.DataStore

class MutablePreferences internal constructor() : Preferences {
    operator fun <T> set(key: Preferences.Key<T>, value: T) {}
    override operator fun <T> get(key: Preferences.Key<T>): T? = null

    override fun toMutablePreferences(): MutablePreferences = this
}

fun Preferences.toMutablePreferences(): MutablePreferences = MutablePreferences()

suspend fun DataStore<Preferences>.edit(
    transform: suspend (MutablePreferences) -> Unit,
) {
    transform(MutablePreferences())
}
