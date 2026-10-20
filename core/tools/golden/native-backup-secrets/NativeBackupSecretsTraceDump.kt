// Golden trace driver for NativeBackupSecretsPolicy (compiled verbatim from the app tree).
//
// A scenario seeds the in-memory settings storage, then runs either capture() or restore(). The
// trace records the returned secrets / written settings and the restore warnings. Maps are kept in
// insertion order so the trace also pins merge ordering, not just the final set of entries.
package native.backup.secrets.golden

import com.newoether.agora.data.ApiKeyEntry
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.EmbeddingModelType
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.McpTransportType
import com.newoether.agora.data.NativeBackupSecrets
import com.newoether.agora.data.NativeBackupSecretsPolicy
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.data.ShellDeviceSecrets
import com.newoether.agora.util.Constants
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
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

private fun JsonObject.required(name: String): JsonElement =
    this[name] ?: error("scenario field `$name` is required")

private fun JsonObject.text(name: String, default: String = ""): String =
    (this[name] as? JsonPrimitive)?.contentOrNull ?: default

private fun JsonObject.number(name: String, default: Int): Int =
    (this[name] as? JsonPrimitive)?.intOrNull ?: default

private fun JsonObject.flag(name: String, default: Boolean): Boolean =
    (this[name] as? JsonPrimitive)?.booleanOrNull ?: default

private fun JsonObject.objectOrEmpty(name: String): JsonObject =
    (this[name] as? JsonObject) ?: JsonObject(emptyMap())

private fun JsonObject.arrayOrEmpty(name: String): JsonArray =
    (this[name] as? JsonArray) ?: JsonArray(emptyList())

private fun JsonObject.stringMap(name: String): Map<String, String> =
    objectOrEmpty(name).mapValues { (_, value) -> value.jsonPrimitive.content }

private fun JsonObject.stringSet(name: String): Set<String> =
    arrayOrEmpty(name).mapTo(linkedSetOf()) { it.jsonPrimitive.content }

private fun parseApiKey(value: JsonElement): ApiKeyEntry {
    val obj = value.jsonObject
    return ApiKeyEntry(
        id = obj.text("id"),
        name = obj.text("name"),
        key = obj.text("key"),
        provider = obj.text("provider", Constants.PROVIDER_GOOGLE),
    )
}

private fun parseShellDevice(value: JsonElement): ShellDeviceConfig {
    val obj = value.jsonObject
    return ShellDeviceConfig(
        id = obj.text("id"),
        name = obj.text("name"),
        description = obj.text("description"),
        type = obj.text("type", "conch"),
        serverUrl = obj.text("serverUrl"),
        apiKey = obj.text("apiKey"),
        conchPublicKey = obj.text("conchPublicKey"),
        sshHost = obj.text("sshHost"),
        sshPort = obj.number("sshPort", 22),
        sshUser = obj.text("sshUser", "root"),
        sshPassword = obj.text("sshPassword"),
        sshHostKey = obj.text("sshHostKey"),
    )
}

private fun parseEmbeddingModel(value: JsonElement): EmbeddingModelConfig {
    val obj = value.jsonObject
    return EmbeddingModelConfig(
        id = obj.text("id"),
        name = obj.text("name"),
        type = EmbeddingModelType.valueOf(obj.text("type", "REMOTE")),
        remoteModelName = obj.text("remoteModelName"),
        remoteBaseUrl = obj.text("remoteBaseUrl"),
        remoteApiKey = obj.text("remoteApiKey"),
        localFilePath = obj.text("localFilePath"),
        batchSize = obj.number("batchSize", 8),
    )
}

private fun parseMcpServer(value: JsonElement): McpServerConfig {
    val obj = value.jsonObject
    return McpServerConfig(
        id = obj.text("id"),
        name = obj.text("name"),
        enabled = obj.flag("enabled", true),
        url = obj.text("url"),
        transport = McpTransportType.valueOf(obj.text("transport", "STREAMABLE_HTTP")),
        headers = obj.stringMap("headers"),
        disabledTools = obj.stringSet("disabledTools"),
    )
}

private fun parseSettings(root: JsonObject): SettingsManager {
    val settings = root.objectOrEmpty("settings")
    return SettingsManager(
        apiKeys = settings.arrayOrEmpty("apiKeys").map(::parseApiKey),
        activeApiKeyIds = settings.stringMap("activeApiKeyIds"),
        webSearchApiKeys = settings.stringMap("webSearchApiKeys"),
        proxyPassword = settings.text("proxyPassword"),
        shellDevices = settings.arrayOrEmpty("shellDevices").map(::parseShellDevice),
        embeddingModels = settings.arrayOrEmpty("embeddingModels").map(::parseEmbeddingModel),
        mcpServers = settings.arrayOrEmpty("mcpServers").map(::parseMcpServer),
    )
}

private fun parseSecrets(root: JsonObject): NativeBackupSecrets {
    val secrets = root.required("secrets").jsonObject
    return NativeBackupSecrets(
        apiKeys = secrets.arrayOrEmpty("apiKeys").map(::parseApiKey),
        activeApiKeyIds = secrets.stringMap("activeApiKeyIds"),
        webSearchApiKeys = secrets.stringMap("webSearchApiKeys"),
        proxyPassword = secrets.text("proxyPassword"),
        shellDevices = secrets.objectOrEmpty("shellDevices").mapValues { (_, value) ->
            val obj = value.jsonObject
            ShellDeviceSecrets(
                apiKey = obj.text("apiKey"),
                sshPassword = obj.text("sshPassword"),
            )
        },
        shellApiKeys = secrets.stringMap("shellApiKeys"),
        embeddingApiKeys = secrets.stringMap("embeddingApiKeys"),
        mcpHeaders = secrets.objectOrEmpty("mcpHeaders").mapValues { (_, value) ->
            value.jsonObject.mapValues { (_, header) -> header.jsonPrimitive.content }
        },
    )
}

private fun JsonObjectBuilder.putStringMap(name: String, values: Map<String, String>) {
    put(name, buildJsonObject { values.forEach { (key, value) -> put(key, value) } })
}

private fun JsonObjectBuilder.putApiKeys(keys: List<ApiKeyEntry>) {
    put("apiKeys", buildJsonArray {
        for (entry in keys) {
            add(buildJsonObject {
                put("id", entry.id)
                put("name", entry.name)
                put("key", entry.key)
                put("provider", entry.provider)
            })
        }
    })
}

private fun secretsJson(secrets: NativeBackupSecrets): JsonObject = buildJsonObject {
    putApiKeys(secrets.apiKeys)
    putStringMap("activeApiKeyIds", secrets.activeApiKeyIds)
    putStringMap("webSearchApiKeys", secrets.webSearchApiKeys)
    put("proxyPassword", secrets.proxyPassword)
    put("shellDevices", buildJsonObject {
        secrets.shellDevices.forEach { (id, entry) ->
            put(id, buildJsonObject {
                put("apiKey", entry.apiKey)
                put("sshPassword", entry.sshPassword)
            })
        }
    })
    putStringMap("shellApiKeys", secrets.shellApiKeys)
    putStringMap("embeddingApiKeys", secrets.embeddingApiKeys)
    put("mcpHeaders", buildJsonObject {
        secrets.mcpHeaders.forEach { (id, headers) ->
            put(id, buildJsonObject { headers.forEach { (key, value) -> put(key, value) } })
        }
    })
}

private suspend fun settingsJson(manager: SettingsManager): JsonObject = buildJsonObject {
    putApiKeys(manager.apiKeys.first())
    putStringMap("activeApiKeyIds", manager.activeApiKeyIds.first())
    putStringMap("webSearchApiKeys", manager.webSearchApiKeys.first())
    put("proxyPassword", manager.proxyPassword.first())
    put("shellDevices", buildJsonArray {
        for (device in manager.shellDevices.first()) {
            add(buildJsonObject {
                put("id", device.id)
                put("name", device.name)
                put("apiKey", device.apiKey)
                put("sshPassword", device.sshPassword)
            })
        }
    })
    put("embeddingModels", buildJsonArray {
        for (model in manager.embeddingModels.first()) {
            add(buildJsonObject {
                put("id", model.id)
                put("type", model.type.name)
                put("remoteApiKey", model.remoteApiKey)
            })
        }
    })
    put("mcpServers", buildJsonArray {
        for (server in manager.mcpServers.first()) {
            add(buildJsonObject {
                put("id", server.id)
                put("headers", buildJsonObject {
                    server.headers.forEach { (key, value) -> put(key, value) }
                })
            })
        }
    })
}

fun main(args: Array<String>) {
    val root = json.parseToJsonElement(File(args[0]).readText()).jsonObject
    val name = root.text("name")
    val op = root.text("op")
    val manager = parseSettings(root)

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "capture" -> {
            val secrets = runBlocking { NativeBackupSecretsPolicy.capture(manager) }
            val rendered = json.encodeToString(secretsJson(secrets))
            out.append("result|").append(esc(rendered)).append("|-\n")
        }
        "restore" -> {
            val secrets = parseSecrets(root)
            val replace = root.flag("replace", false)
            val warnings = runBlocking {
                NativeBackupSecretsPolicy.restore(secrets, manager, replace)
            }
            val warningsJson = buildJsonArray { warnings.forEach { add(it) } }
            val rendered = runBlocking { json.encodeToString(settingsJson(manager)) }
            out.append("warnings|").append(esc(json.encodeToString(warningsJson))).append("|-\n")
            out.append("result|").append(esc(rendered)).append("|-\n")
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
