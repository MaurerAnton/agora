package com.newoether.agora.util

import android.util.Base64
import com.sun.net.httpserver.HttpServer
import java.io.IOException
import java.net.InetSocketAddress
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import kotlinx.serialization.json.*
import okhttp3.OkHttpClient
import okhttp3.Request
import okio.Buffer
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class ConchHttpFramingTest {
    private val key = ByteArray(32) { it.toByte() }

    private fun frame(kind: String, sequence: Long, body: ByteArray = byteArrayOf()): String {
        val value = buildJsonObject {
            put("_conch_event", kind)
            put("_conch_sequence", sequence)
            put("body", Base64.encodeToString(body, Base64.NO_WRAP))
        }
        return "event: $kind\ndata: ${ShellCrypto.encrypt(key, value.toString().toByteArray())}\n\n"
    }

    private fun body(wire: String) = ConchHttpBodySource(ConchHttpFrames(Buffer().writeUtf8(wire), key.copyOf()))

    @Test fun postTerminalFramesAndGarbageFailEveryRead() {
        for (suffix in listOf(frame("http_body", 1, byteArrayOf(1)), frame("http_end", 1), "garbage", "\n")) {
            body(frame("http_end", 0) + suffix).use { source ->
                repeat(2) {
                    assertTrue("Trailing bytes must not become EOF", runCatching { source.read(Buffer(), 1) }.exceptionOrNull() is IOException)
                }
            }
        }
    }

    @Test fun crossRequestFrameCannotBecomeBodyBytes() {
        val wire = frame("http_body", 0, byteArrayOf(1)) + frame("http_end", 1)
        val otherKey = ByteArray(32) { (it + 1).toByte() }
        ConchHttpBodySource(ConchHttpFrames(Buffer().writeUtf8(wire), otherKey)).use { source ->
            val sink = Buffer()
            assertTrue(runCatching { source.read(sink, 1) }.exceptionOrNull() is IOException)
            assertEquals(0L, sink.size)
        }
    }

    @Test fun emptyResponseRequiresTerminalAndOuterBodyEnd() {
        body(frame("http_end", 0)).use { source ->
            assertEquals(0L, source.read(Buffer(), 0))
            assertEquals(-1L, source.read(Buffer(), 1))
            assertEquals(-1L, source.read(Buffer(), 1))
        }
        body("").use { source ->
            assertTrue(runCatching { source.read(Buffer(), 1) }.exceptionOrNull() is IOException)
        }
    }

    @Test fun terminalChecksHttpBodyBoundaryWithoutWaitingForConnectionClose() {
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val peers = mutableListOf<InetSocketAddress>()
        server.createContext("/") { exchange ->
            peers.add(exchange.remoteAddress)
            exchange.sendResponseHeaders(200, 0)
            exchange.responseBody.use { it.write(frame("http_end", 0).toByteArray()) }
        }
        server.start()
        val client = OkHttpClient.Builder().readTimeout(2, TimeUnit.SECONDS).build()
        try {
            repeat(2) {
                client.newCall(Request.Builder().url("http://127.0.0.1:${server.address.port}/").build()).execute().use { response ->
                    ConchHttpBodySource(ConchHttpFrames(response.body.source(), key.copyOf())).use { source ->
                        assertEquals(-1L, source.read(Buffer(), 1))
                    }
                }
            }
            assertEquals(2, peers.size)
            assertEquals(peers[0], peers[1])
        } finally {
            client.connectionPool.evictAll()
            server.stop(0)
        }
    }

    @Test fun partialOuterBodyWaitsForCompletionAndCallCancellationUnblocksRead() {
        val release = CountDownLatch(1)
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        server.createContext("/") { exchange ->
            try {
                val wire = frame("http_end", 0).toByteArray()
                exchange.sendResponseHeaders(200, (wire.size + 100).toLong())
                exchange.responseBody.write(wire)
                exchange.responseBody.flush()
                release.await(5, TimeUnit.SECONDS)
            } finally {
                exchange.close()
            }
        }
        server.start()
        val client = OkHttpClient.Builder().readTimeout(0, TimeUnit.SECONDS).build()
        val call = client.newCall(Request.Builder().url("http://127.0.0.1:${server.address.port}/").build())
        val executor = Executors.newSingleThreadExecutor()
        try {
            call.execute().use { response ->
                ConchHttpBodySource(ConchHttpFrames(response.body.source(), key.copyOf())).use { source ->
                    val read = executor.submit<Throwable?> { runCatching { source.read(Buffer(), 1) }.exceptionOrNull() }
                    assertThrows(java.util.concurrent.TimeoutException::class.java) { read.get(100, TimeUnit.MILLISECONDS) }
                    call.cancel()
                    assertTrue(read.get(2, TimeUnit.SECONDS) is IOException)
                }
            }
        } finally {
            call.cancel()
            release.countDown()
            executor.shutdownNow()
            client.connectionPool.evictAll()
            server.stop(0)
        }
    }
}
