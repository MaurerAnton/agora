// Split from DataImporterTraceDump.kt: settings/memory/skill/files trace rendering.
package data.importer.golden

import android.content.ContentResolver
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.newoether.agora.data.ApiKeyEntry
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.DataExporter
import com.newoether.agora.data.DataImporter
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.MemoryManager
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.data.SkillManager
import com.newoether.agora.data.SystemPromptEntry
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.ConversationSettingsTransferCoordinator
import com.newoether.agora.data.repository.SettingsRepository
import com.newoether.agora.util.readFontMeta
import java.io.File
import java.nio.file.Files
import java.sql.Connection
import java.sql.DriverManager
import java.sql.ResultSet
import java.util.Base64
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

// ---- settings view -----------------------------------------------------------------------------

internal fun EmbeddingModelConfig.toJson(): JsonObject = buildJsonObject {
    put("id", id)
    put("name", name)
    put("type", type.name)
    put("remoteModelName", remoteModelName)
    put("remoteBaseUrl", remoteBaseUrl)
    put("remoteApiKey", remoteApiKey)
    put("localFilePath", localFilePath)
    put("batchSize", batchSize)
}

internal fun ShellDeviceConfig.toJson(): JsonObject = buildJsonObject {
    put("id", id)
    put("name", name)
    put("description", description)
    put("type", type)
    put("serverUrl", serverUrl)
    put("apiKey", apiKey)
    put("conchPublicKey", conchPublicKey)
    put("sshHost", sshHost)
    put("sshPort", sshPort)
    put("sshUser", sshUser)
    put("sshPassword", sshPassword)
    put("sshHostKey", sshHostKey)
}

internal fun McpServerConfig.toJson(): JsonObject = buildJsonObject {
    put("id", id)
    put("name", name)
    put("enabled", enabled)
    put("url", url)
    put("transport", transport.name)
    put("headers", buildJsonObject { headers.forEach { (key, value) -> put(key, value) } })
    put("disabledTools", buildJsonArray { disabledTools.forEach { add(it) } })
}

internal fun CustomProviderConfig.toJson(): JsonObject = buildJsonObject {
    put("name", name)
    put("protocol", protocol.wireValue)
    put("id", id)
    put("legacyNames", buildJsonArray { legacyNames.forEach { add(it) } })
    put("responsesApiEnabled", responsesApiEnabled)
    put("anthropicCacheEnabled", anthropicCacheEnabled)
    put("anthropicCacheTtl", anthropicCacheTtl)
}

internal fun promptJson(prompt: SystemPromptEntry): JsonObject = buildJsonObject {
    put("id", prompt.id)
    put("title", prompt.title)
    put("content", prompt.content)
    fun items(name: String, values: List<com.newoether.agora.data.PromptTemplateItem>) {
        put(name, buildJsonArray {
            values.forEach { item ->
                add(buildJsonObject {
                    put("id", item.id)
                    put("type", item.type.name)
                    put("value", item.value)
                })
            }
        })
    }
    items("systemItems", prompt.systemItems)
    items("userItems", prompt.userItems)
    items("assistantItems", prompt.assistantItems)
    items("userPrependItems", prompt.userPrependItems)
    items("userPostpendItems", prompt.userPostpendItems)
}

internal fun conversationSettingsJson(settings: ConversationSettings): JsonObject = buildJsonObject {
    put("contextWindow", settings.contextWindow)
    put("temperature", settings.temperature?.toString())
    put("maxTokens", settings.maxTokens)
    put("topP", settings.topP?.toString())
    put("frequencyPenalty", settings.frequencyPenalty?.toString())
    put("presencePenalty", settings.presencePenalty?.toString())
    put("codeExecutionEnabled", settings.codeExecutionEnabled)
    put("googleSearchEnabled", settings.googleSearchEnabled)
    put("openAiWebSearchEnabled", settings.openAiWebSearchEnabled)
    put("thinkingEnabled", settings.thinkingEnabled)
    put("thinkingLevel", settings.thinkingLevel)
    put("thinkingBudgetEnabled", settings.thinkingBudgetEnabled)
    put("thinkingBudgetTokens", settings.thinkingBudgetTokens)
    put("openAiServiceTierEnabled", settings.openAiServiceTierEnabled)
    put("openAiServiceTier", settings.openAiServiceTier)
    put("webSearchEnabled", settings.webSearchEnabled)
    put("shellEnabled", settings.shellEnabled)
    put("lowContextModeEnabled", settings.lowContextModeEnabled)
}

internal fun ApiKeyEntry.toJson(): JsonObject = buildJsonObject {
    put("id", id)
    put("name", name)
    put("key", key)
    put("provider", provider)
}

internal fun settingsViewJson(settings: SettingsManager): JsonObject = buildJsonObject {
    put("systemPrompts", buildJsonArray {
        settings.systemPromptsSnapshot().forEach { add(promptJson(it)) }
    })
    put("embeddingModels", buildJsonArray {
        settings.embeddingModelsSnapshot().forEach { add(it.toJson()) }
    })
    put("activeEmbeddingModelId", settings.stringSnapshot("activeEmbeddingModelId"))
    put("fontPreference", settings.stringSnapshot("fontPreference"))
    put("customFontPath", settings.stringSnapshot("customFontPath"))
    put("customFontName", settings.stringSnapshot("customFontName"))
    put("activeSystemPromptId", settings.nullableStringSnapshot("activeSystemPromptId"))
    put("shellDevices", buildJsonArray {
        settings.shellDevicesSnapshot().forEach { add(it.toJson()) }
    })
    put("mcpServers", buildJsonArray {
        settings.mcpServersSnapshot().forEach { add(it.toJson()) }
    })
    put("customProviders", buildJsonArray {
        settings.customProvidersSnapshot().forEach { add(it.toJson()) }
    })
    put("conversationSettings", buildJsonObject {
        settings.conversationSettingsSnapshot().forEach { (id, value) ->
            put(id, conversationSettingsJson(value))
        }
    })
    put("apiKeys", buildJsonArray { settings.apiKeysSnapshot().forEach { add(it.toJson()) } })
    put("activeApiKeyIds", buildJsonObject {
        settings.activeApiKeyIdsSnapshot().forEach { (key, value) -> put(key, value) }
    })
    put("webSearchApiKeys", buildJsonObject {
        settings.webSearchApiKeysSnapshot().forEach { (key, value) -> put(key, value) }
    })
    put("proxyPassword", settings.proxyPasswordSnapshot())
}

// ---- memory / skill / files --------------------------------------------------------------------

internal fun memoryJson(memory: MemoryManager): JsonObject = buildJsonObject {
    put("active", memory.getActiveMemory())
    put("meta", memory.getMetaJson())
    put("files", buildJsonArray {
        memory.listFiles().forEach { info ->
            add(buildJsonObject {
                put("name", info.name)
                put("description", info.description)
                put("content", memory.readFile(info.name))
            })
        }
    })
}

internal fun skillsJson(skills: SkillManager): JsonObject = buildJsonObject {
    put("meta", skills.getMetaJson())
    put("files", buildJsonArray {
        skills.listFiles().forEach { info ->
            add(buildJsonObject {
                put("name", info.name)
                put("description", info.description)
                put("content", skills.readFile(info.name))
            })
        }
    })
}

internal fun filesJson(filesDir: File): JsonArray = buildJsonArray {
    val rootPath = filesDir.toPath()
    filesDir.walkTopDown()
        .filter { it.isFile }
        .sortedBy { it.relativeTo(filesDir).invariantSeparatorsPath }
        .forEach { file ->
            add(buildJsonObject {
                put("name", rootPath.relativize(file.toPath()).toString().replace(File.separatorChar, '/'))
                put("base64", Base64.getEncoder().encodeToString(file.readBytes()))
            })
        }
}

