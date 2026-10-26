// Split from DataImporterTraceDump.kt: SettingsManager snapshot construction and archive writing.
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

// ---- settings snapshot (the data-exporter stand's parser) --------------------------------------

internal fun buildSettings(
    root: JsonObject,
    conversationSettings: Map<String, ConversationSettings>,
    systemPrompts: List<SystemPromptEntry>,
    apiKeys: List<ApiKeyEntry>,
    activeApiKeyIds: Map<String, String>,
    webSearchApiKeys: Map<String, String>,
    proxyPassword: String,
): SettingsManager = SettingsManager(
    strings = mapOf(
        "selectedModel" to value<String>(root, "selectedModel"),
        "contextCompactPrompt" to value<String>(root, "contextCompactPrompt"),
        "thinkingLevel" to value<String>(root, "thinkingLevel"),
        "openAiServiceTier" to value<String>(root, "openAiServiceTier"),
        "anthropicCacheTtl" to value<String>(root, "anthropicCacheTtl"),
        "titleGenerationPrompt" to value<String>(root, "titleGenerationPrompt"),
        "modelSearchMethod" to value<String>(root, "modelSearchMethod"),
        "manualSearchMethod" to value<String>(root, "manualSearchMethod"),
        "appLanguage" to value<String>(root, "appLanguage"),
        "webSearchProvider" to value<String>(root, "webSearchProvider"),
        "webSearchBaseUrl" to value<String>(root, "webSearchBaseUrl"),
        "imageGenSize" to value<String>(root, "imageGenSize"),
        "imageTranscriptionPrompt" to value<String>(root, "imageTranscriptionPrompt"),
        "themeMode" to value<String>(root, "themeMode"),
        "colorScheme" to value<String>(root, "colorScheme"),
        "schemeStyle" to value<String>(root, "schemeStyle"),
        "fontPreference" to value<String>(root, "fontPreference"),
        "proxyType" to value<String>(root, "proxyType"),
        "proxyHost" to value<String>(root, "proxyHost"),
        "proxyPort" to value<String>(root, "proxyPort"),
        "proxyUsername" to value<String>(root, "proxyUsername"),
        "proxyBypass" to value<String>(root, "proxyBypass"),
        "toolCallDisplayMode" to value<String>(root, "toolCallDisplayMode"),
        "thinkingSegmentDisplayMode" to value<String>(root, "thinkingSegmentDisplayMode"),
        "activeEmbeddingModelId" to value<String>(root, "activeEmbeddingModelId"),
        "customFontName" to value<String>(root, "customFontName"),
        "customFontPath" to value<String>(root, "customFontPath"),
    ),
    nullableStrings = mapOf(
        "contextCompactModel" to nullableValue<String>(root, "contextCompactModel"),
        "titleGenerationModel" to nullableValue<String>(root, "titleGenerationModel"),
        "imageGenModel" to nullableValue<String>(root, "imageGenModel"),
        "imageTranscriptionModel" to nullableValue<String>(root, "imageTranscriptionModel"),
        "activeSystemPromptId" to nullableValue<String>(root, "activeSystemPromptId"),
    ),
    booleans = mapOf(
        "accessActiveMemory" to value<Boolean>(root, "accessActiveMemory"),
        "accessPastConversations" to value<Boolean>(root, "accessPastConversations"),
        "accessSavedMemories" to value<Boolean>(root, "accessSavedMemories"),
        "accessSkills" to value<Boolean>(root, "accessSkills"),
        "amoledEnabled" to value<Boolean>(root, "amoledEnabled"),
        "anthropicCacheEnabled" to value<Boolean>(root, "anthropicCacheEnabled"),
        "askUserEnabled" to value<Boolean>(root, "askUserEnabled"),
        "autoCacheEnabled" to value<Boolean>(root, "autoCacheEnabled"),
        "autoExpandActiveGroup" to value<Boolean>(root, "autoExpandActiveGroup"),
        "automationToolsEnabled" to value<Boolean>(root, "automationToolsEnabled"),
        "automationWakeLockEnabled" to value<Boolean>(root, "automationWakeLockEnabled"),
        "autoUpdateCheck" to value<Boolean>(root, "autoUpdateCheck"),
        "autoWrapCodeBlocks" to value<Boolean>(root, "autoWrapCodeBlocks"),
        "blurEffectsEnabled" to value<Boolean>(root, "blurEffectsEnabled"),
        "codeExecutionEnabled" to value<Boolean>(root, "codeExecutionEnabled"),
        "contextCompactEnabled" to value<Boolean>(root, "contextCompactEnabled"),
        "contextCompactPreserveSystemPrompt" to value<Boolean>(root, "contextCompactPreserveSystemPrompt"),
        "detailedTokenUsage" to value<Boolean>(root, "detailedTokenUsage"),
        "dynamicColor" to value<Boolean>(root, "dynamicColor"),
        "exactExecutionEnabled" to value<Boolean>(root, "exactExecutionEnabled"),
        "googleSearchEnabled" to value<Boolean>(root, "googleSearchEnabled"),
        "hapticsEnabled" to value<Boolean>(root, "hapticsEnabled"),
        "imageGenEnabled" to value<Boolean>(root, "imageGenEnabled"),
        "imageTranscriptionEnabled" to value<Boolean>(root, "imageTranscriptionEnabled"),
        "openAiResponsesApiEnabled" to value<Boolean>(root, "openAiResponsesApiEnabled"),
        "openAiServiceTierEnabled" to value<Boolean>(root, "openAiServiceTierEnabled"),
        "parseInlineDollarMath" to value<Boolean>(root, "parseInlineDollarMath"),
        "proxyEnabled" to value<Boolean>(root, "proxyEnabled"),
        "ragSearchEnabled" to value<Boolean>(root, "ragSearchEnabled"),
        "reduceMotion" to value<Boolean>(root, "reduceMotion"),
        "shellConfirmEnabled" to value<Boolean>(root, "shellConfirmEnabled"),
        "shellEnabled" to value<Boolean>(root, "shellEnabled"),
        "showDocumentationFab" to value<Boolean>(root, "showDocumentationFab"),
        "showUncachedNotification" to value<Boolean>(root, "showUncachedNotification"),
        "stickToBottom" to value<Boolean>(root, "stickToBottom"),
        "thinkingBudgetEnabled" to value<Boolean>(root, "thinkingBudgetEnabled"),
        "thinkingEnabled" to value<Boolean>(root, "thinkingEnabled"),
        "titleGenerationEnabled" to value<Boolean>(root, "titleGenerationEnabled"),
        "titleGenerationNotificationsEnabled" to value<Boolean>(root, "titleGenerationNotificationsEnabled"),
        "visualizeContextRollout" to value<Boolean>(root, "visualizeContextRollout"),
        "webSearchEnabled" to value<Boolean>(root, "webSearchEnabled"),
    ),
    ints = mapOf(
        "contextCompactRetainCount" to value<Int>(root, "contextCompactRetainCount"),
        "contextCompactThresholdPercent" to value<Int>(root, "contextCompactThresholdPercent"),
        "imageTranscriptionBatchSize" to value<Int>(root, "imageTranscriptionBatchSize"),
        "maxContextWindow" to value<Int>(root, "maxContextWindow"),
        "searchContextWindow" to value<Int>(root, "searchContextWindow"),
        "searchMatchLimit" to value<Int>(root, "searchMatchLimit"),
        "thinkingBudgetTokens" to value<Int>(root, "thinkingBudgetTokens"),
        "webSearchNumResults" to value<Int>(root, "webSearchNumResults"),
    ),
    nullableInts = mapOf(
        "defaultMaxTokens" to nullableValue<Int>(root, "defaultMaxTokens"),
    ),
    floats = mapOf(
        "ragThreshold" to value<Float>(root, "ragThreshold"),
    ),
    nullableFloats = mapOf(
        "defaultTemperature" to nullableValue<Float>(root, "defaultTemperature"),
        "defaultTopP" to nullableValue<Float>(root, "defaultTopP"),
        "defaultFrequencyPenalty" to nullableValue<Float>(root, "defaultFrequencyPenalty"),
        "defaultPresencePenalty" to nullableValue<Float>(root, "defaultPresencePenalty"),
    ),
    stringSets = mapOf(
        "customModels" to value(root, "customModels"),
        "enabledModels" to value(root, "enabledModels"),
        "imageTranscriptionEnabledModels" to value(root, "imageTranscriptionEnabledModels"),
    ),
    stringMaps = mapOf(
        "modelAliases" to value(root, "modelAliases"),
        "providerBaseUrls" to value(root, "providerBaseUrls"),
    ),
    boolMaps = mapOf(
        "modelProviderNames" to value(root, "modelProviderNames"),
    ),
    embeddingModels = value(root, "embeddingModels"),
    shellDevices = value(root, "shellDevices"),
    mcpServers = value(root, "mcpServers"),
    customProviders = value<List<CustomProviderConfig>>(root, "customProviders"),
    conversationSettings = conversationSettings,
    systemPrompts = systemPrompts,
    apiKeys = apiKeys,
    activeApiKeyIds = activeApiKeyIds,
    webSearchApiKeys = webSearchApiKeys,
    proxyPassword = proxyPassword,
)

// ---- archive construction ----------------------------------------------------------------------

internal fun writeArchive(entries: JsonArray, target: File) {
    target.parentFile?.mkdirs()
    ZipOutputStream(target.outputStream().buffered()).use { zip ->
        for (element in entries) {
            val entry = element.jsonObject
            val name = entry.string("name")
            val bytes = entry["text"]?.let { (it as JsonPrimitive).content.toByteArray(Charsets.UTF_8) }
                ?: entry["base64"]?.let { Base64.getDecoder().decode((it as JsonPrimitive).content) }
                ?: error("archive entry $name needs text or base64")
            zip.putNextEntry(ZipEntry(name))
            zip.write(bytes)
            zip.closeEntry()
        }
    }
}
