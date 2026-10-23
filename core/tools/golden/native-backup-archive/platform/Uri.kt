// Stand-local android.net.Uri: the archive only keeps the Uri opaque and hands it to
// ContentResolver.openFileDescriptor, so only the string round-trip matters.
package android.net

class Uri private constructor(private val value: String) {
    override fun toString(): String = value

    companion object {
        @JvmStatic
        fun parse(value: String): Uri = Uri(value)
    }
}
