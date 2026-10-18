// JVM stand-in for kotlinx.coroutines.flow.StateFlow used by MemoryManager.kt.
//
// MemoryManager only uses these as monotonic revision counters (reads `.value`, never collected),
// so an aliasing value holder is a faithful substitute: `asStateFlow()` keeps observing the same
// backing counter. The code under test is compiled verbatim from the app tree; nothing here
// duplicates its logic.
package kotlinx.coroutines.flow

class MutableStateFlow<T>(initialValue: T) {
    var value: T = initialValue
}

class StateFlow<T> internal constructor(private val source: MutableStateFlow<T>) {
    val value: T get() = source.value
}

fun <T> MutableStateFlow<T>.asStateFlow(): StateFlow<T> = StateFlow(this)
