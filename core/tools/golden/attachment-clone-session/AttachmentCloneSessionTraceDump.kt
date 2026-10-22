// Golden trace driver for data/MessageAttachmentCloneSession.kt (lifted verbatim with a widened
// visibility) and the lifted MessageEntity.
//
// The driver clones real files in a temporary directory, so the copy, the per-owner isolation, the
// metadata rewrite and the rollback are all observed for real. The random file name is masked in
// the trace (the port takes the name generator as a parameter).
package attachment.clone.session.golden

import com.newoether.agora.data.MessageAttachmentCloneSession
import com.newoether.agora.data.cloneAttachmentMeta
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

private val UUID_MASK =
    Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")

/** The workspace prefix is machine dependent, so it is masked together with the random id. */
private fun mask(value: String): String =
    UUID_MASK.replace(value.replace(workspacePrefix, "<workspace>"), "<uuid>")

private val workspacePrefix: String = File("platform_clone_workspace").absolutePath

private fun buildMessage(spec: JsonObject): MessageEntity = MessageEntity(
    id = (spec["id"] as JsonPrimitive).content,
    conversationId = (spec["conversationId"] as? JsonPrimitive)?.contentOrNull ?: "c1",
    parentId = (spec["parentId"] as? JsonPrimitive)?.contentOrNull,
    text = (spec["text"] as? JsonPrimitive)?.contentOrNull ?: "",
    images = (spec["images"] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList(),
    status = MessageStatus.SUCCESS,
    participant = Participant.USER,
    timestamp = (spec["timestamp"] as? JsonPrimitive)?.contentOrNull?.toLong() ?: 0L,
    runId = (spec["runId"] as? JsonPrimitive)?.contentOrNull ?: "run-1",
    attachmentMeta = (spec["attachmentMeta"] as? JsonPrimitive)?.contentOrNull,
)

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
    val workspace = File("platform_clone_workspace")
    workspace.deleteRecursively()
    workspace.mkdirs()
    val destination = File(workspace, "clones")
    val sources = File(workspace, "sources")
    sources.mkdirs()

    if (root["sources"] is JsonArray) {
        for (element in root["sources"] as JsonArray) {
            val spec = element.jsonObject
            val name = (spec["name"] as JsonPrimitive).content
            val content = (spec["content"] as? JsonPrimitive)?.contentOrNull ?: ""
            File(sources, name).writeText(content)
        }
    }

    val session = MessageAttachmentCloneSession(destination)
    val clones = buildJsonArray {
        if (root["messages"] is JsonArray) {
            for (element in root["messages"] as JsonArray) {
                val spec = element.jsonObject
                val message = buildMessage(spec)
                val ownerKey = (spec["ownerKey"] as? JsonPrimitive)?.contentOrNull
                val clone = if (ownerKey != null) {
                    session.cloneMessage(message, ownerKey)
                } else {
                    session.cloneMessage(message)
                }
                add(buildJsonObject {
                    put("id", clone.id)
                    put("images", buildJsonArray { clone.images.forEach { add(mask(it)) } })
                    put("attachmentMeta", clone.attachmentMeta?.let { mask(it) })
                })
            }
        }
        if (root["standaloneMeta"] != null) {
            val spec = root["standaloneMeta"]!!.jsonObject
            val raw = (spec["raw"] as JsonPrimitive).content
            val clone = cloneAttachmentMeta(raw) { path ->
                session.cloneBackingPath("owner", path)
            }
            add(buildJsonObject { put("standaloneMeta", mask(clone)) })
        }
    }

    if ((root["commit"] as? JsonPrimitive)?.content == "true") {
        session.commit()
    } else {
        session.rollback()
    }

    val trace = buildJsonObject {
        put("clones", clones)
        put("cloneDirectoryExists", destination.exists())
        put("cloneFiles", buildJsonArray {
            if (destination.exists()) {
                destination.listFiles()?.sortedBy { it.name }?.forEach { file ->
                    add(buildJsonObject {
                        put("name", mask(file.name))
                        put("content", file.readText())
                    })
                }
            }
        })
        put("sourceFiles", buildJsonArray {
            sources.listFiles()?.sortedBy { it.name }?.forEach { file ->
                add(buildJsonObject {
                    put("name", file.name)
                    put("content", file.readText())
                })
            }
        })
    }
    workspace.deleteRecursively()
    return Json.encodeToString(JsonObject.serializer(), trace)
}
