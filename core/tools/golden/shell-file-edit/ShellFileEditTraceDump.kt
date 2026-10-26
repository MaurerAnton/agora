// Golden trace driver for the pure file helpers extracted from util/ShellClient.kt.
//
// ShellClient.kt is the Conch shell transport object. The top of the file holds transport-free
// policy: the file-edit matcher/replacer, line counting, SHA-256, line-ending preference and the
// bounded shell-output reader. The driver replays one JSON scenario per file and prints the
// canonical trace; the C++ port is compared byte for byte.
package shell.file.edit.golden

import com.newoether.agora.util.SHELL_COMMAND_OUTPUT_MAX_BYTES
import com.newoether.agora.util.ShellFileEditResult
import com.newoether.agora.util.completeUtf8PrefixLength
import com.newoether.agora.util.editShellFileContent
import com.newoether.agora.util.fileEditMatchRanges
import com.newoether.agora.util.preferredFileEditLineEnding
import com.newoether.agora.util.readBoundedShellOutput
import com.newoether.agora.util.replaceFileEditMatches
import com.newoether.agora.util.shellFileLineCount
import com.newoether.agora.util.shellFileSha256
import com.newoether.agora.util.withFileEditLineEnding
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStream
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

private val json = Json

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

private fun render(scenarioFile: File): String {
    val scenario = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = scenario.string("op")
    return buildJsonObject {
        put("op", op)
        when (op) {
            "lineCount" -> {
                val content = scenario.string("content")
                put("content", content)
                put("outcome", buildJsonObject { put("count", shellFileLineCount(content)) })
            }
            "sha256" -> {
                val bytesHex = scenario.string("bytesHex")
                put("bytesHex", bytesHex)
                put("outcome", buildJsonObject { put("sha256", shellFileSha256(hexBytes(bytesHex))) })
            }
            "completeUtf8Prefix" -> {
                val bytesHex = scenario.string("bytesHex")
                put("bytesHex", bytesHex)
                put("outcome", buildJsonObject {
                    put("length", completeUtf8PrefixLength(hexBytes(bytesHex)))
                })
            }
            "readBoundedShellOutput" -> {
                val bytesHex = scenario.string("bytesHex")
                val maxBytes = if (scenario.containsKey("maxBytes")) {
                    scenario.int("maxBytes")
                } else {
                    SHELL_COMMAND_OUTPUT_MAX_BYTES
                }
                val chunks = scenario.intList("chunks")
                put("bytesHex", bytesHex)
                put("maxBytes", maxBytes)
                put("chunks", buildJsonArray { for (chunk in chunks) add(chunk) })
                val outcome = try {
                    val bytes = hexBytes(bytesHex)
                    val stream: InputStream =
                        if (chunks.isEmpty()) ByteArrayInputStream(bytes)
                        else ChunkedInputStream(bytes, chunks)
                    val (content, truncated) = stream.readBoundedShellOutput(maxBytes)
                    buildJsonObject {
                        put("content", content)
                        put("truncated", truncated)
                    }
                } catch (failure: Throwable) {
                    errorJson(failure)
                }
                put("outcome", outcome)
            }
            "matchRanges" -> {
                val content = scenario.string("content")
                val oldString = scenario.string("oldString")
                put("content", content)
                put("oldString", oldString)
                val outcome = try {
                    val ranges = fileEditMatchRanges(content, oldString)
                    buildJsonObject {
                        put("ranges", buildJsonArray {
                            for (range in ranges) {
                                add(buildJsonArray {
                                    add(range.first)
                                    add(range.last)
                                })
                            }
                        })
                    }
                } catch (failure: Throwable) {
                    errorJson(failure)
                }
                put("outcome", outcome)
            }
            "replaceMatches" -> {
                val content = scenario.string("content")
                val matches = scenario.intPairs("matches").map { (first, last) -> IntRange(first, last) }
                val newString = scenario.string("newString")
                put("content", content)
                put("matches", buildJsonArray {
                    for (match in matches) {
                        add(buildJsonArray {
                            add(match.first)
                            add(match.last)
                        })
                    }
                })
                put("newString", newString)
                val outcome = try {
                    buildJsonObject {
                        put("content", replaceFileEditMatches(content, matches, newString))
                    }
                } catch (failure: Throwable) {
                    errorJson(failure)
                }
                put("outcome", outcome)
            }
            "edit" -> {
                val content = scenario.string("content")
                val oldString = scenario.string("oldString")
                val newString = scenario.string("newString")
                val replaceAll = scenario.boolean("replaceAll")
                put("content", content)
                put("oldString", oldString)
                put("newString", newString)
                put("replaceAll", replaceAll)
                val outcome = try {
                    val (edited, result) = editShellFileContent(
                        content,
                        oldString,
                        newString,
                        replaceAll,
                    )
                    buildJsonObject {
                        put("content", edited)
                        put("result", editResultJson(result))
                    }
                } catch (failure: Throwable) {
                    errorJson(failure)
                }
                put("outcome", outcome)
            }
            "preferredLineEnding" -> {
                val text = scenario.string("text")
                put("text", text)
                put("outcome", buildJsonObject {
                    put("lineEnding", preferredFileEditLineEnding(text))
                })
            }
            "withLineEnding" -> {
                val text = scenario.string("text")
                val lineEnding = scenario.string("lineEnding")
                put("text", text)
                put("lineEnding", lineEnding)
                put("outcome", buildJsonObject {
                    put("content", text.withFileEditLineEnding(lineEnding))
                })
            }
            else -> error("unknown op $op")
        }
    }.toString()
}

private fun editResultJson(result: ShellFileEditResult): JsonObject = buildJsonObject {
    put("replacements", result.replacements)
    put("sha256", result.sha256)
    put("error", result.error)
}

private fun errorJson(failure: Throwable): JsonObject = buildJsonObject {
    put("error", buildJsonObject {
        put("type", failure.javaClass.simpleName)
        put("message", failure.message ?: "")
    })
}

private fun hexBytes(value: String): ByteArray {
    require(value.length % 2 == 0) { "odd hex length" }
    return ByteArray(value.length / 2) { index ->
        value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}

private class ChunkedInputStream(
    private val bytes: ByteArray,
    private val chunks: List<Int>,
) : InputStream() {
    private var position = 0
    private var chunkIndex = 0

    override fun read(): Int {
        if (position >= bytes.size) return -1
        return bytes[position++].toInt() and 0xff
    }

    override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
        if (position >= bytes.size) return -1
        val requested =
            if (chunkIndex < chunks.size) chunks[chunkIndex++] else bytes.size - position
        val count = minOf(requested, length, bytes.size - position)
        if (count <= 0) return 0
        bytes.copyInto(buffer, offset, position, position + count)
        position += count
        return count
    }
}

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.int(name: String): Int =
    (this[name] as? JsonPrimitive)?.content?.toIntOrNull() ?: error("missing int $name")

private fun JsonObject.boolean(name: String): Boolean =
    (this[name] as? JsonPrimitive)?.content?.toBooleanStrictOrNull()
        ?: error("missing boolean $name")

private fun JsonObject.intList(name: String): List<Int> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { it.jsonPrimitive.content.toInt() }
}

private fun JsonObject.intPairs(name: String): List<Pair<Int, Int>> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { element ->
        val pair = element.jsonArray
        pair[0].jsonPrimitive.content.toInt() to pair[1].jsonPrimitive.content.toInt()
    }
}
