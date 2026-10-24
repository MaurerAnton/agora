// Platform substitute for androidx.datastore.preferences.core, as consumed by the settings
// contracts and the schema key declarations. Only key identity and typed reads are needed; the
// settings storage itself is replaced by the in-memory SettingsManager below.
package androidx.datastore.preferences.core

interface Preferences {
    operator fun <T> get(key: Key<T>): T?

    data class Key<T>(val name: String)
}

fun stringPreferencesKey(name: String): Preferences.Key<String> = Preferences.Key(name)
fun booleanPreferencesKey(name: String): Preferences.Key<Boolean> = Preferences.Key(name)
fun intPreferencesKey(name: String): Preferences.Key<Int> = Preferences.Key(name)
fun longPreferencesKey(name: String): Preferences.Key<Long> = Preferences.Key(name)
fun stringSetPreferencesKey(name: String): Preferences.Key<Set<String>> = Preferences.Key(name)
fun floatPreferencesKey(name: String): Preferences.Key<Float> = Preferences.Key(name)
