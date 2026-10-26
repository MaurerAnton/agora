// Stand-local subset of okio used by ConchNetwork.kt and BoundedWireRead.kt.
//
// The real transport is okio; this stand substitutes the buffered-source contract with a
// deterministic byte source so every bounded-read decision is replayable offline. The subset is
// exactly the surface the two compiled sources touch: `request`, `readUtf8`, `exhausted`,
// `readUtf8LineStrict`, `buffer.size` and `Timeout.timeout`.
package okio

import java.io.EOFException
import java.util.concurrent.TimeUnit

class Buffer(initialSize: Long = 0) {
    var size: Long = initialSize
}

interface BufferedSource {
    val buffer: Buffer
    fun request(byteCount: Long): Boolean
    fun readUtf8(): String
    fun exhausted(): Boolean
    fun readUtf8LineStrict(limit: Long): String
}

class Timeout {
    var timeoutMillis: Long = 0
        private set

    fun timeout(timeout: Long, unit: TimeUnit): Timeout {
        timeoutMillis = unit.toMillis(timeout)
        return this
    }
}

/**
 * Deterministic stand-in for an okio BufferedSource.
 *
 * `declaredSize` is the number of bytes the source claims to hold; it may exceed `bytes.size`
 * so a scenario can pin an over-limit rejection without materialising a 40 MiB body. When the
 * bounded read asks for more bytes than remain, `request` answers from `declaredSize` and the
 * read is rejected before `readUtf8` is called.
 */
class ByteSource(
    private val bytes: ByteArray,
    private val declaredSize: Long = bytes.size.toLong(),
) : BufferedSource {
    private var position = 0L
    override val buffer = Buffer(declaredSize)

    override fun request(byteCount: Long): Boolean {
        require(byteCount >= 0)
        return declaredSize - position >= byteCount
    }

    override fun exhausted(): Boolean = declaredSize - position <= 0

    override fun readUtf8(): String {
        val start = position.coerceAtMost(bytes.size.toLong()).toInt()
        position = declaredSize
        buffer.size = 0
        return String(bytes, start, bytes.size - start, Charsets.UTF_8)
    }

    override fun readUtf8LineStrict(limit: Long): String {
        require(limit >= 0)
        val start = position.toInt()
        if (start >= bytes.size) throw EOFException("no newline")
        var newline = -1
        var index = start
        while (index < bytes.size) {
            if (bytes[index] == '\n'.code.toByte()) {
                newline = index
                break
            }
            index++
        }
        if (newline < 0 || newline - start > limit) throw EOFException("no newline within $limit")
        var end = newline
        if (end > start && bytes[end - 1] == '\r'.code.toByte()) end--
        position = (newline + 1).toLong()
        buffer.size = declaredSize - position
        return String(bytes, start, end - start, Charsets.UTF_8)
    }
}
