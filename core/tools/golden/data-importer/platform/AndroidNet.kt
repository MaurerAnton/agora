// Stand-local android.net.Uri.
//
// DataExporter only reads `scheme` (to route content:// and file:// through the ContentResolver)
// and passes the whole Uri back to the resolver. Android's parser is permissive: it never throws
// for arbitrary strings, and a scheme is the leading `[A-Za-z][A-Za-z0-9+.-]*` before ':',
// lowercased. That is the complete surface this stand needs.
package android.net

class Uri private constructor(private val raw: String) {
    val scheme: String? = run {
        val colon = raw.indexOf(':')
        if (colon <= 0) {
            null
        } else {
            val candidate = raw.substring(0, colon)
            if (
                candidate.first().isLetter() &&
                candidate.all { it.isLetterOrDigit() || it == '+' || it == '-' || it == '.' }
            ) {
                candidate.lowercase()
            } else {
                null
            }
        }
    }

    override fun toString(): String = raw

    companion object {
        @JvmStatic
        fun parse(uriString: String): Uri = Uri(uriString)
    }
}
