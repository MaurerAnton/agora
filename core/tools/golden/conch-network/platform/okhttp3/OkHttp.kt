// Stand-local subset of OkHttp 5 used by ConchNetwork.kt.
//
// ConchNetwork builds one request, applies headers with `Request.Builder.header` (case-insensitive
// replace), sets the per-call timeout and reads the response through okio. The stand keeps those
// semantics — including `Headers.get` returning the last matching value and `isSuccessful` being
// 200..299 — and records the request instead of opening a socket. Sockets, TLS, redirects and
// connection pooling stay platform concerns.
package okhttp3

import java.io.Closeable
import java.util.concurrent.TimeUnit
import okio.BufferedSource
import okio.ByteSource
import okio.Timeout

class MediaType(val text: String) {
    override fun toString(): String = text

    companion object {
        fun String.toMediaType(): MediaType = MediaType(this)
    }
}

class RequestBody(private val contentType: MediaType?, private val text: String) {
    fun contentType(): MediaType? = contentType
    fun text(): String = text

    companion object {
        fun String.toRequestBody(contentType: MediaType? = null): RequestBody =
            RequestBody(contentType, this)
    }
}

class Headers private constructor(private val values: List<Pair<String, String>>) {
    fun get(name: String): String? =
        values.lastOrNull { (key, _) -> key.equals(name, ignoreCase = true) }?.second

    fun toList(): List<Pair<String, String>> = values

    class Builder {
        private val values = mutableListOf<Pair<String, String>>()

        fun set(name: String, value: String): Builder {
            values.removeAll { (key, _) -> key.equals(name, ignoreCase = true) }
            values.add(name to value)
            return this
        }

        fun build(): Headers = Headers(values.toList())
    }
}

class Request private constructor(
    val url: String,
    val method: String,
    val headers: Headers,
    val body: RequestBody?,
) {
    class Builder {
        private var url = ""
        private var method = "GET"
        private val headers = Headers.Builder()
        private var body: RequestBody? = null

        fun url(url: String): Builder {
            this.url = url
            return this
        }

        fun get(): Builder {
            method = "GET"
            body = null
            return this
        }

        fun post(body: RequestBody): Builder {
            method = "POST"
            this.body = body
            return this
        }

        fun header(name: String, value: String): Builder {
            headers.set(name, value)
            return this
        }

        fun build(): Request = Request(url, method, headers.build(), body)
    }
}

class ConnectionPool(
    val maxIdleConnections: Int,
    keepAliveDuration: Long,
    timeUnit: TimeUnit,
) {
    val keepAliveDurationMillis: Long = timeUnit.toMillis(keepAliveDuration)
}

class ResponseBody(private val source: BufferedSource) {
    fun source(): BufferedSource = source
}

class Response internal constructor(
    val code: Int,
    private val headers: Headers,
    val body: ResponseBody,
    private val onClose: () -> Unit,
) : Closeable {
    val isSuccessful: Boolean get() = code in 200..299

    fun header(name: String): String? = headers.get(name)

    override fun close() {
        onClose()
    }
}

interface Call {
    fun execute(): Response
    fun timeout(): Timeout
    fun isCanceled(): Boolean = false

    interface Factory {
        fun newCall(request: Request): Call
    }
}

class RecordedRequest(
    val method: String,
    val url: String,
    val headers: List<Pair<String, String>>,
    val body: String?,
    val contentType: String?,
    val timeoutMillis: Long,
)

class ScriptedResponse(
    val code: Int,
    val body: ByteArray,
    val declaredSize: Long,
    val headers: List<Pair<String, String>>,
)

/** One global script shared by the stand HttpClient and every client built from it. */
object ScriptedTransport {
    var response: ScriptedResponse = ScriptedResponse(200, ByteArray(0), 0, emptyList())
    var lastRequest: RecordedRequest? = null
    var lastClosed: Boolean = false

    fun reset() {
        response = ScriptedResponse(200, ByteArray(0), 0, emptyList())
        lastRequest = null
        lastClosed = false
    }
}

private class StandCall(
    private val request: Request,
    private val script: ScriptedTransport,
) : Call {
    private val timeout = Timeout()

    override fun timeout(): Timeout = timeout

    override fun execute(): Response {
        script.lastRequest = RecordedRequest(
            method = request.method,
            url = request.url,
            headers = request.headers.toList(),
            body = request.body?.text(),
            contentType = request.body?.contentType()?.toString(),
            timeoutMillis = timeout.timeoutMillis,
        )
        val scripted = script.response
        val headers = Headers.Builder().apply {
            scripted.headers.forEach { (name, value) -> set(name, value) }
        }.build()
        return Response(
            code = scripted.code,
            headers = headers,
            body = ResponseBody(ByteSource(scripted.body, scripted.declaredSize)),
            onClose = { script.lastClosed = true },
        )
    }
}

class OkHttpClient internal constructor(
    val retryOnConnectionFailure: Boolean,
    val followRedirects: Boolean,
    val followSslRedirects: Boolean,
    val connectionPool: ConnectionPool,
    private val script: ScriptedTransport,
) : Call.Factory {
    constructor() : this(
        retryOnConnectionFailure = true,
        followRedirects = true,
        followSslRedirects = true,
        connectionPool = ConnectionPool(5, 5, TimeUnit.MINUTES),
        script = ScriptedTransport,
    )

    fun newBuilder(): Builder = Builder(this)

    override fun newCall(request: Request): Call = StandCall(request, script)

    class Builder(private val base: OkHttpClient) {
        private var retryOnConnectionFailure = base.retryOnConnectionFailure
        private var followRedirects = base.followRedirects
        private var followSslRedirects = base.followSslRedirects
        private var connectionPool = base.connectionPool

        fun retryOnConnectionFailure(value: Boolean): Builder {
            retryOnConnectionFailure = value
            return this
        }

        fun followRedirects(value: Boolean): Builder {
            followRedirects = value
            return this
        }

        fun followSslRedirects(value: Boolean): Builder {
            followSslRedirects = value
            return this
        }

        fun connectionPool(value: ConnectionPool): Builder {
            connectionPool = value
            return this
        }

        fun build(): OkHttpClient = OkHttpClient(
            retryOnConnectionFailure = retryOnConnectionFailure,
            followRedirects = followRedirects,
            followSslRedirects = followSslRedirects,
            connectionPool = connectionPool,
            script = base.script,
        )
    }
}
