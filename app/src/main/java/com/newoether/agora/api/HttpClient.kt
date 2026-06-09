package com.newoether.agora.api

import okhttp3.MediaType.Companion.toMediaType
import com.newoether.agora.util.DebugLog
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okio.BufferedSource
import java.net.InetSocketAddress
import java.net.Proxy
import java.util.concurrent.TimeUnit

object HttpClient {
    private val JSON = "application/json; charset=utf-8".toMediaType()

    @Volatile
    private var socksProxyHost: String? = null
    @Volatile
    private var socksProxyPort: Int = 9050

    private fun buildClient(): OkHttpClient {
        val builder = OkHttpClient.Builder()
            .connectTimeout(30, TimeUnit.SECONDS)
            .readTimeout(5, TimeUnit.MINUTES)
            .writeTimeout(30, TimeUnit.SECONDS)
        val host = socksProxyHost
        if (!host.isNullOrBlank()) {
            val proxy = Proxy(Proxy.Type.SOCKS, InetSocketAddress(host, socksProxyPort))
            builder.proxy(proxy)
        }
        return builder.build()
    }

    @Volatile
    var client: OkHttpClient = buildClient()
        private set

    fun setSocksProxy(host: String?, port: Int) {
        socksProxyHost = host
        socksProxyPort = port
        client = buildClient()
        DebugLog.d("HttpClient", if (host.isNullOrBlank()) "SOCKS proxy disabled" else "SOCKS proxy set to $host:$port")
    }

    fun disableProxy() {
        setSocksProxy(null, 9050)
    }

    class StreamHandle(private val response: okhttp3.Response) {
        val code: Int get() = response.code
        val source: BufferedSource? get() = response.body?.source()
        val errorBody: String? by lazy {
            try { response.body?.string() } catch (_: Exception) { null }
        }
        fun close() = response.close()
        fun readLine(): String? = source?.readUtf8Line()
    }

    fun streamPost(url: String, jsonBody: String, headers: Map<String, String> = emptyMap()): StreamHandle {
        val body = jsonBody.toRequestBody(JSON)
        val requestBuilder = Request.Builder().url(url).post(body)
        headers.forEach { (k, v) -> requestBuilder.addHeader(k, v) }
        return StreamHandle(client.newCall(requestBuilder.build()).execute())
    }

    fun post(url: String, jsonBody: String, headers: Map<String, String> = emptyMap()): String? {
        val body = jsonBody.toRequestBody(JSON)
        val requestBuilder = Request.Builder().url(url).post(body)
        headers.forEach { (k, v) -> requestBuilder.addHeader(k, v) }
        val response = client.newCall(requestBuilder.build()).execute()
        return response.use {
            if (it.isSuccessful) it.body?.string()
            else {
                DebugLog.e("HttpClient", "POST $url failed: ${it.code} ${it.body?.string()}")
                null
            }
        }
    }

    fun fetchModels(url: String, headers: Map<String, String> = emptyMap()): String? {
        val requestBuilder = Request.Builder().url(url).get()
        headers.forEach { (k, v) -> requestBuilder.addHeader(k, v) }
        val response = client.newCall(requestBuilder.build()).execute()
        return response.use {
            if (it.isSuccessful) it.body?.string() else null
        }
    }
}
