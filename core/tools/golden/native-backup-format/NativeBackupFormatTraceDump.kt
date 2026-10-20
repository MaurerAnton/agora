// Golden trace driver for NativeBackupFormat (compiled verbatim from the app tree).
package native.backup.format.golden

import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.EmbeddingModelType
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.McpTransportType
import com.newoether.agora.data.NativeBackupFormat
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.data.asPortableRemoteConfig
import com.newoether.agora.data.withoutSecrets
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.File

private val json = Json

private fun esc(value: String): String {
    val out = StringBuilder(value.length)
    for (c in value) {
        when (c) {
            '\\' -> out.append("\\\\")
            '|' -> out.append("\\p")
            ';' -> out.append("\\s")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

private fun JsonObject.text(name: String, default: String = ""): String =
    (this[name] as? JsonPrimitive)?.contentOrNull ?: default

private fun JsonObject.number(name: String, default: Int): Int =
    (this[name] as? JsonPrimitive)?.intOrNull ?: default

private fun JsonObject.flag(name: String, default: Boolean): Boolean =
    (this[name] as? JsonPrimitive)?.booleanOrNull ?: default

private fun JsonObject.objectOrEmpty(name: String): JsonObject =
    (this[name] as? JsonObject) ?: JsonObject(emptyMap())

private fun JsonObject.arrayOrEmpty(name: String) =
    (this[name] as? kotlinx.serialization.json.JsonArray) ?: kotlinx.serialization.json.JsonArray(emptyList())

private fun JsonObject.stringMap(name: String): Map<String, String> =
    objectOrEmpty(name).mapValues { (_, value) -> value.jsonPrimitive.content }

private fun parseShell(value: JsonObject): ShellDeviceConfig = ShellDeviceConfig(
    id = value.text("id"),
    name = value.text("name"),
    description = value.text("description"),
    type = value.text("type", "conch"),
    serverUrl = value.text("serverUrl"),
    apiKey = value.text("apiKey"),
    conchPublicKey = value.text("conchPublicKey"),
    sshHost = value.text("sshHost"),
    sshPort = value.number("sshPort", 22),
    sshUser = value.text("sshUser", "root"),
    sshPassword = value.text("sshPassword"),
    sshHostKey = value.text("sshHostKey"),
)

private fun parseEmbedding(value: JsonObject): EmbeddingModelConfig = EmbeddingModelConfig(
    id = value.text("id"),
    name = value.text("name"),
    type = EmbeddingModelType.valueOf(value.text("type", "REMOTE")),
    remoteModelName = value.text("remoteModelName"),
    remoteBaseUrl = value.text("remoteBaseUrl"),
    remoteApiKey = value.text("remoteApiKey"),
    localFilePath = value.text("localFilePath"),
    batchSize = value.number("batchSize", 8),
)

private fun parseMcp(value: JsonObject): McpServerConfig = McpServerConfig(
    id = value.text("id"),
    name = value.text("name"),
    enabled = value.flag("enabled", true),
    url = value.text("url"),
    transport = McpTransportType.valueOf(value.text("transport", "STREAMABLE_HTTP")),
    headers = value.stringMap("headers"),
    disabledTools = value.arrayOrEmpty("disabledTools")
        .mapTo(linkedSetOf()) { it.jsonPrimitive.content },
)

fun main(args: Array<String>) {
    val root = json.parseToJsonElement(File(args[0]).readText()).jsonObject
    val name = root.text("name")
    val op = root.text("op")
    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    val result: JsonElement = when (op) {
        "format" -> buildJsonObject {
            put("currentVersion", NativeBackupFormat.CURRENT_VERSION)
            put("minSupportedVersion", NativeBackupFormat.MIN_SUPPORTED_VERSION)
            put("separateSecretsSinceVersion", NativeBackupFormat.SEPARATE_SECRETS_SINCE_VERSION)
            put("incrementalBaselineRevision", NativeBackupFormat.INCREMENTAL_BASELINE_REVISION)
            put("manifestEntry", NativeBackupFormat.MANIFEST_ENTRY)
            put("conversationsEntry", NativeBackupFormat.CONVERSATIONS_ENTRY)
            put("conversationIndexEntry", NativeBackupFormat.CONVERSATION_INDEX_ENTRY)
            put("conversationEntryPrefix", NativeBackupFormat.CONVERSATION_ENTRY_PREFIX)
            put("tasksEntry", NativeBackupFormat.TASKS_ENTRY)
            put("settingsEntry", NativeBackupFormat.SETTINGS_ENTRY)
            put("legacyExtraSettingsEntry", NativeBackupFormat.LEGACY_EXTRA_SETTINGS_ENTRY)
            put("secretsEntry", NativeBackupFormat.SECRETS_ENTRY)
            put("systemPromptsEntry", NativeBackupFormat.SYSTEM_PROMPTS_ENTRY)
            put("customFontEntry", NativeBackupFormat.CUSTOM_FONT_ENTRY)
            put("imageMediaPrefix", NativeBackupFormat.IMAGE_MEDIA_PREFIX)
            put("videoMediaPrefix", NativeBackupFormat.VIDEO_MEDIA_PREFIX)
            put("draftMediaPrefix", NativeBackupFormat.DRAFT_MEDIA_PREFIX)
            put("supported", buildJsonArray {
                for (version in -2..7) add(NativeBackupFormat.isSupported(version))
            })
        }
        "entry" -> buildJsonArray {
            for (element in root.arrayOrEmpty("ids")) add(NativeBackupFormat.conversationEntry(element.jsonPrimitive.content))
        }
        "models" -> {
            val settings = root.objectOrEmpty("settings")
            buildJsonObject {
                put("shell", parseShell(settings.objectOrEmpty("shell")).withoutSecrets().let {
                    buildJsonObject {
                        put("id", it.id)
                        put("name", it.name)
                        put("apiKey", it.apiKey)
                        put("sshPassword", it.sshPassword)
                    }
                })
                put("mcp", parseMcp(settings.objectOrEmpty("mcp")).withoutSecrets().let {
                    buildJsonObject {
                        put("id", it.id)
                        put("headers", buildJsonObject { it.headers.forEach { (key, value) -> put(key, value) } })
                    }
                })
                val portable = parseEmbedding(settings.objectOrEmpty("embedding")).asPortableRemoteConfig()
                if (portable == null) {
                    put("embedding", JsonNull)
                } else {
                    put("embedding", buildJsonObject {
                        put("id", portable.id)
                        put("type", portable.type.name)
                        put("remoteApiKey", portable.remoteApiKey)
                        put("localFilePath", portable.localFilePath)
                    })
                }
            }
        }
        else -> error("unknown op: $op")
    }
    out.append("result|").append(esc(json.encodeToString(result))).append("|-\n")
    print(out)
}
