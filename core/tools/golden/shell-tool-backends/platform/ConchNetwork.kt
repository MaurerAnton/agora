package com.newoether.agora.util

import java.security.KeyFactory
import java.security.spec.PKCS8EncodedKeySpec

/**
 * Stand-local scripted double for util/ConchNetwork.kt (the OkHttp/okio transport).
 * ShellClient.kt compiles VERBATIM against this surface: ConchTextResponse plus the
 * getTextResponse/postTextResponse entry points, with a recorded request log.
 *
 * Responses come from the scenario script:
 *  - transport failures are thrown as the mapped exception type (UnknownHost, Connect,
 *    SocketTimeout, InterruptedIO, SSL, plain);
 *  - on a successful encrypted call (X-Encryption header present and 2xx) the double seals
 *    the scripted plaintext with the EXACT session key ShellClient's encryptedPost derived —
 *    X25519 is symmetric, so the shared secret recomputed from the fixed stand server private
 *    key and the request's X-Client-Public-Key equals the client's — and can sign it with the
 *    real ShellCrypto.responseSignature, so ShellClient's verify/decrypt path runs against
 *    genuine crypto;
 *  - non-2xx and plain (blank api key) bodies pass through verbatim, keeping HTTP-error
 *    messages deterministic;
 *  - a `dynamicHandshake` body is produced by the driver's builder from the request's
 *    challenge (the HMAC covers a random per-run challenge).
 */
internal data class ConchTextResponse(
    val code: Int,
    val body: String,
    val isSuccessful: Boolean,
    val signature: String,
)

internal object ConchNetwork {
    /** The wire limits ShellToolBackends passes into streamPost (real ConchNetwork consts). */
    const val CONTROL_LIMIT = 64L shl 10
    const val LINE_LIMIT = 10L shl 20

    /** ShellToolBackends passes this as streamPost's callClient; the stand never dials. */
    val streamCalls: Any get() = Unit

    /** One scripted transport call, recorded for the trace. */
    internal class RecordedRequest(
        val url: String,
        val method: String,
        val body: String,
        val headers: List<Pair<String, String>>,
        val callTimeoutMillis: Long?,
    )

    /** What the scenario wants the wire to return. */
    internal class ScriptedResponse(
        val code: Int = 200,
        /** Plaintext body: returned verbatim when not sealing, sealed when sealing. */
        val body: String = "",
        /** Overrides the wire body after sealing is decided (decrypt-failure input). */
        val rawBody: String? = null,
        /** "valid" signs the wire body with the derived session key; "invalid" uses zeros. */
        val signatureMode: String = "absent",
        /** Produces the handshake JSON from the request's ?challenge= value. */
        val dynamicHandshake: Boolean = false,
        /** Transport failure kind: unknown_host | connect | socket_timeout | interrupted_io | ssl | plain. */
        val failure: String? = null,
        val failureDetail: String = "",
    )

    /** The stand's fixed server identity (the driver mirrors these constants). */
    internal const val SERVER_PRIVATE_KEY_PKCS8 =
        "MC4CAQAwBQYDK2VuBCIEIHFn0CpncaiXmIxD1seMg1VuVjEsnCQC7We5bp39n6wL"

    val requests = mutableListOf<RecordedRequest>()
    val queue = ArrayDeque<ScriptedResponse>()

    /** Built by the driver: handshake JSON for a given challenge (signed with the scenario key). */
    var handshakeBuilder: (String) -> String = { error("handshake builder not installed") }

    fun reset() {
        requests.clear()
        queue.clear()
        handshakeBuilder = { error("handshake builder not installed") }
    }

    fun getTextResponse(url: String, headers: Map<String, String>): ConchTextResponse {
        requests.add(RecordedRequest(url, "GET", "", headers.map { it.toPair() }, null))
        return respond()
    }

    fun postTextResponse(
        url: String,
        body: String,
        headers: Map<String, String>,
        callTimeoutMillis: Long?,
    ): ConchTextResponse {
        requests.add(
            RecordedRequest(url, "POST", body, headers.map { it.toPair() }, callTimeoutMillis),
        )
        return respond()
    }

    private fun respond(): ConchTextResponse {
        if (queue.isEmpty()) error("conch script exhausted")
        val scripted = queue.removeFirst()
        scripted.failure?.let { throw failureFor(scripted.failure, scripted.failureDetail) }

        val requestHeaders = requests.last().headers.toMap()
        val encrypted = requestHeaders.containsKey("X-Encryption")
        val seal = encrypted && scripted.code in 200..299

        var wireBody = scripted.body
        var sessionKey: ByteArray? = null
        if (seal) {
            sessionKey = deriveSessionKey(requestHeaders)
            wireBody = ShellCrypto.encrypt(sessionKey, scripted.body.toByteArray(Charsets.UTF_8))
        }
        scripted.rawBody?.let { wireBody = it }

        // A sealed response is authenticatable by default ("invalid" opts out explicitly);
        // plain responses never carry a signature the client checks.
        val signature = when {
            scripted.signatureMode == "invalid" -> "0".repeat(64)
            seal -> ShellCrypto.responseSignature(sessionKey!!, scripted.code, wireBody)
            scripted.signatureMode == "valid" ->
                ShellCrypto.responseSignature(
                    deriveSessionKey(requestHeaders), scripted.code, wireBody,
                )
            else -> ""
        }
        if (scripted.dynamicHandshake) {
            wireBody = handshakeBuilder(challengeOf(requests.last().url))
        }
        return ConchTextResponse(
            code = scripted.code,
            body = wireBody,
            isSuccessful = scripted.code in 200..299,
            signature = signature,
        )
    }

    private fun challengeOf(url: String): String =
        url.substringAfter("challenge=", "")

    /**
     * The shared secret both sides compute: client private × server public equals
     * server private × client public (the request's X-Client-Public-Key).
     */
    private fun deriveSessionKey(requestHeaders: Map<String, String>): ByteArray {
        val clientPublicKey = requestHeaders["X-Client-Public-Key"]
            ?: error("missing X-Client-Public-Key")
        val serverPrivate = KeyFactory.getInstance("X25519").generatePrivate(
            PKCS8EncodedKeySpec(
                android.util.Base64.decode(
                    SERVER_PRIVATE_KEY_PKCS8,
                    android.util.Base64.URL_SAFE or android.util.Base64.NO_WRAP,
                ),
            ),
        )
        return ShellCrypto.deriveAesKey(serverPrivate, ShellCrypto.decodePublicKey(clientPublicKey))
    }

    private fun failureFor(kind: String, detail: String): Exception = when (kind) {
        "unknown_host" -> java.net.UnknownHostException(detail)
        "connect" -> java.net.ConnectException(detail)
        "socket_timeout" -> java.net.SocketTimeoutException(detail)
        "interrupted_io" -> java.io.InterruptedIOException(detail)
        "ssl" -> javax.net.ssl.SSLException(detail)
        else -> Exception(detail)
    }
}
