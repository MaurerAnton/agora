// Platform substitute for util/ShellClient.kt: the sandbox stands only carry
// ShellClient.GrepMatch through their result types (the grep projection), while the real file
// pulls the whole Conch transport (ShellCrypto, ConchNetwork). The nested declaration mirrors
// the app one so the reference syntax and the field shape stay identical.
package com.newoether.agora.util

object ShellClient {
    data class GrepMatch(
        val path: String,
        val line: Int,
        val content: String,
        val before: List<String> = emptyList(),
        val after: List<String> = emptyList(),
    )
}
