// Platform substitute for androidx.datastore.preferences.core, as consumed by
// data/PortableSettingsReset.kt and data/SettingsPreferenceSchema.kt.
//
// The reset policy only removes keys and writes two empty containers, so the substitute records
// the surviving keys in insertion order and the driver renders the outcome.
package androidx.datastore.preferences.core

interface Preferences {
    operator fun <T> get(key: Key<T>): T?

    data class Key<T>(val name: String)
}

interface MutablePreferences : Preferences {
    fun <T> remove(key: Preferences.Key<T>): MutablePreferences
    operator fun <T> set(key: Preferences.Key<T>, value: T): MutablePreferences
}

/** The in-memory store the driver seeds and inspects. */
class InMemoryPreferences : MutablePreferences {
    private val values = linkedMapOf<String, Any?>()

    /** The keys that were removed, in call order. */
    val removals = mutableListOf<String>()

    /** The keys that were written, in call order. */
    val writes = mutableListOf<Pair<String, String>>()

    /** The keys that are present, in insertion order. */
    val names: List<String> get() = values.keys.toList()

    fun put(name: String, value: Any?) {
        values[name] = value
    }

    override fun <T> get(key: Preferences.Key<T>): T? {
        @Suppress("UNCHECKED_CAST")
        return values[key.name] as T?
    }

    override fun <T> remove(key: Preferences.Key<T>): MutablePreferences {
        values.remove(key.name)
        removals += key.name
        return this
    }

    override fun <T> set(key: Preferences.Key<T>, value: T): MutablePreferences {
        values[key.name] = value
        writes += key.name to value.toString()
        return this
    }
}

fun stringPreferencesKey(name: String): Preferences.Key<String> = Preferences.Key(name)
fun booleanPreferencesKey(name: String): Preferences.Key<Boolean> = Preferences.Key(name)
fun intPreferencesKey(name: String): Preferences.Key<Int> = Preferences.Key(name)
fun longPreferencesKey(name: String): Preferences.Key<Long> = Preferences.Key(name)
fun stringSetPreferencesKey(name: String): Preferences.Key<Set<String>> = Preferences.Key(name)
fun floatPreferencesKey(name: String): Preferences.Key<Float> = Preferences.Key(name)
