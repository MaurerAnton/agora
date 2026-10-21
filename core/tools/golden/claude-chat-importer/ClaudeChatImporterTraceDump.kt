// Golden trace driver for data/ClaudeChatImporter.kt (compiled verbatim from the app tree).
//
// A scenario feeds the importer the bytes of a Claude export — a raw JSON document or a ZIP
// archive (the driver builds the archive and freezes it as fixtures/<stem>.zip so the C++ port
// reads the same bytes). The trace contains the preview and the import format, or the failure.
package claude.chat.importer.golden

import com.newoether.agora.data.ClaudeChatImporter
import java.io.ByteArrayInputStream
import java.io.File
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

/** A fixed DOS timestamp keeps the built archive byte-identical between runs. */
private const val FIXED_TIME = 1_000_000_000_000L

private fun buildArchive(entries: JsonArray): ByteArray {
    val out = java.io.ByteArrayOutputStream()
    ZipOutputStream(out).use { stream ->
        for (element in entries) {
            val spec = element.jsonObject
            val name = (spec["name"] as JsonPrimitive).content
            val content = (spec["content"] as? JsonPrimitive)?.content.orEmpty().toByteArray()
            val entry = ZipEntry(name)
            entry.time = FIXED_TIME
            if ((spec["stored"] as? JsonPrimitive)?.content == "true") {
                entry.method = ZipEntry.STORED
                val crc = CRC32()
                crc.update(content)
                entry.size = content.size.toLong()
                entry.compressedSize = content.size.toLong()
                entry.crc = crc.value
            }
            stream.putNextEntry(entry)
            stream.write(content)
            stream.closeEntry()
        }
    }
    return out.toByteArray()
}

private fun renderTimestamp(value: Long, before: Long, after: Long): JsonElement =
    if (value in (before - 1_000)..(after + 1_000)) {
        JsonPrimitive("<now>")
    } else {
        JsonPrimitive(value)
    }

private fun renderPreview(preview: ClaudeChatImporter.ImportPreview, before: Long, after: Long): JsonObject =
    buildJsonObject {
        put("conversations", buildJsonArray {
            preview.conversations.forEach { summary ->
                add(buildJsonObject {
                    put("uuid", summary.uuid)
                    put("title", summary.title)
                    put("messageCount", summary.messageCount)
                })
            }
        })
        put("conversationCount", preview.conversationCount)
        put("totalMessageCount", preview.totalMessageCount)
        put("humanMessageCount", preview.humanMessageCount)
        put("assistantMessageCount", preview.assistantMessageCount)
        put("hasAttachments", preview.hasAttachments)
    }

private fun renderImport(imported: ClaudeChatImporter.ImportConversations, before: Long, after: Long): JsonObject =
    buildJsonObject {
        put("conversations", buildJsonArray {
            imported.conversations.forEach { chat ->
                add(buildJsonObject {
                    put("id", chat.id)
                    put("title", chat.title)
                    put("lastUpdated", renderTimestamp(chat.lastUpdated, before, after))
                    put("selectedBranchesJson", chat.selectedBranchesJson)
                    put("systemPromptId", chat.systemPromptId)
                    put("modelId", chat.modelId)
                })
            }
        })
        put("messages", buildJsonArray {
            imported.messages.forEach { message ->
                add(buildJsonObject {
                    put("id", message.id)
                    put("conversationId", message.conversationId)
                    put("parentId", message.parentId)
                    put("text", message.text)
                    put("images", buildJsonArray { message.images.forEach { add(it) } })
                    put("thoughts", message.thoughts)
                    put("thoughtTitle", message.thoughtTitle)
                    put("tokenCount", message.tokenCount)
                    put("status", message.status)
                    put("participant", message.participant)
                    put("timestamp", renderTimestamp(message.timestamp, before, after))
                    put("thoughtTimeMs", message.thoughtTimeMs)
                    put("modelName", message.modelName)
                    put("toolCallJson", message.toolCallJson)
                    put("attachmentMeta", message.attachmentMeta)
                })
            }
        })
    }

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
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
    val root = Json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val bytes: ByteArray = when {
        root["zip"] != null -> {
            val archive = buildArchive(root["zip"] as JsonArray)
            val fixtures = File(scenarioFile.parentFile.parentFile, "fixtures")
            fixtures.mkdirs()
            File(fixtures, scenarioFile.nameWithoutExtension + ".zip").writeBytes(archive)
            archive
        }
        root["text"] != null -> (root["text"] as JsonPrimitive).content.toByteArray()
        else -> error("scenario needs `zip` or `text`")
    }
    val selectedIds: Set<String>? = (root["selectedIds"] as? JsonArray)?.mapNotNull {
        (it as? JsonPrimitive)?.contentOrNull
    }?.toSet()

    val before = System.currentTimeMillis()
    val importer = ClaudeChatImporter()
    val result = importer.extractAndParse { ByteArrayInputStream(bytes) }
    val after = System.currentTimeMillis()

    val trace: JsonObject = result.fold(
        onSuccess = { conversations ->
            buildJsonObject {
                put("preview", renderPreview(importer.preview(conversations), before, after))
                put("import", renderImport(importer.toImportFormat(conversations, selectedIds), before, after))
            }
        },
        onFailure = { error ->
            buildJsonObject {
                put("error", if (error is kotlinx.serialization.SerializationException) {
                    buildJsonObject { put("type", "decode") }
                } else {
                    buildJsonObject {
                        put("type", error::class.simpleName ?: error.javaClass.name)
                        put("message", error.message ?: "")
                    }
                })
            }
        },
    )
    return Json.encodeToString(JsonObject.serializer(), trace)
}
