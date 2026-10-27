package com.newoether.agora.api

import com.newoether.agora.util.ShellCrypto

/**
 * Stand-local scripted double of the api/HttpClient.kt streaming surface used by
 * tool/ShellToolBackends.kt, which compiles VERBATIM against this shape: boundStreamScope(),
 * streamPost(...) and StreamHandle { code, errorBody, readLine(), close() }.
 *
 * The double records every streamPost invocation (url, headers, maxLineBytes/maxErrorBytes —
 * pinning how the backend calls the client, including the ConchNetwork limit constants), then
 * answers from the script: a transport failure thrown as the mapped java.net/SSL exception (so
 * describeConchRequestFailure runs for real), or a handle serving the scripted lines and error
 * body. When the request carries X-Encryption, each scripted `data: …` payload is sealed with
 * the session key recomputed from the fixed stand server private key and the request's
 * X-Client-Public-Key (X25519 symmetry), so ShellClient's decryptSseData path runs for real on
 * the read lines. Random ciphertext is never traced (the trace records the invocation, not the
 * wire body).
 */
object HttpClient {
    class ScriptedCall(
        val url: String,
        val headers: Map<String, String>,
        val maxLineBytes: Long?,
        val maxErrorBytes: Long?,
    )

    class ScriptedResponse(
        val code: Int = 200,
        val errorBody: String? = null,
        /** Plaintext lines; data payloads are sealed automatically when encrypted. */
        val lines: List<String> = emptyList(),
        val failure: String? = null,
        val failureDetail: String = "",
    )

    val calls = mutableListOf<ScriptedCall>()
    val queue = ArrayDeque<ScriptedResponse>()
    var closed = false

    fun reset() {
        calls.clear()
        queue.clear()
        closed = false
    }

    fun boundStreamScope(): Any? = null

    fun streamPost(
        url: String,
        jsonBody: String,
        headers: Map<String, String> = emptyMap(),
        scope: Any?,
        callClient: Any? = null,
        maxLineBytes: Long? = null,
        maxErrorBytes: Long? = null,
    ): StreamHandle {
        calls.add(ScriptedCall(url, headers, maxLineBytes, maxErrorBytes))
        if (queue.isEmpty()) error("http stream script exhausted")
        val scripted = queue.removeFirst()
        scripted.failure?.let { throw failureFor(scripted.failure, scripted.failureDetail) }
        val encrypted = headers.containsKey("X-Encryption")
        val wireLines = scripted.lines.map { line ->
            if (encrypted && line.startsWith("data: ")) {
                "data: " + seal(headers, line.substring("data: ".length))
            } else {
                line
            }
        }
        return StreamHandle(scripted.code, scripted.errorBody, wireLines)
    }

    /** The stand's fixed server identity (mirrors the shell-client stand). */
    internal const val SERVER_PRIVATE_KEY_PKCS8 =
        "MC4CAQAwBQYDK2VuBCIEIHFn0CpncaiXmIxD1seMg1VuVjEsnCQC7We5bp39n6wL"

    private fun seal(headers: Map<String, String>, plaintext: String): String {
        val clientPublicKey = headers["X-Client-Public-Key"] ?: error("missing client key")
        val serverPrivate = java.security.KeyFactory.getInstance("X25519").generatePrivate(
            java.security.spec.PKCS8EncodedKeySpec(
                android.util.Base64.decode(
                    SERVER_PRIVATE_KEY_PKCS8,
                    android.util.Base64.URL_SAFE or android.util.Base64.NO_WRAP,
                ),
            ),
        )
        val sessionKey = ShellCrypto.deriveAesKey(
            serverPrivate, ShellCrypto.decodePublicKey(clientPublicKey),
        )
        return ShellCrypto.encrypt(sessionKey, plaintext.toByteArray(Charsets.UTF_8))
    }

    private fun failureFor(kind: String, detail: String): Exception = when (kind) {
        "unknown_host" -> java.net.UnknownHostException(detail)
        "connect" -> java.net.ConnectException(detail)
        "socket_timeout" -> java.net.SocketTimeoutException(detail)
        "interrupted_io" -> java.io.InterruptedIOException(detail)
        "ssl" -> javax.net.ssl.SSLException(detail)
        else -> Exception(detail)
    }

    class StreamHandle internal constructor(
        val code: Int,
        val errorBody: String?,
        private val lines: List<String>,
    ) {
        private var position = 0

        fun readLine(): String? {
            if (position >= lines.size) return null
            return lines[position++]
        }

        fun close() {
            closed = true
        }
    }
}
