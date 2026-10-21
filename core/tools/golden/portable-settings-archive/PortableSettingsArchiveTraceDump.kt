// Golden trace driver for data/PortableSettingsArchive.kt, compiled verbatim from the app tree.
//
// op=export  -> prints Json.encodeToString(PortableSettingsArchive.toJsonObject(...)) exactly.
// op=restore -> prints {"calls":[...],"warnings":[...]} for every SettingsManager save, plus the
//               error category and message when the archive throws.
//
// The scenario supplies the complete settings snapshot (every flow key, typed), so the double
// never invents a default, and the two injected callbacks of the real call site:
// resolveSystemPromptId (promptImport::resolve) and the custom-font file check (java.io.File).
package portable.settings.archive.golden

import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.PortableSettingsArchive
import com.newoether.agora.data.RestoredCustomFont
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.ShellDeviceConfig
import java.io.File
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

private fun required(root: JsonObject, key: String): JsonElement =
    root[key] ?: error("settings snapshot is missing `$key`")

private inline fun <reified T> value(root: JsonObject, key: String): T =
    json.decodeFromJsonElement(required(root, key))

private inline fun <reified T> nullableValue(root: JsonObject, key: String): T? {
    val element = required(root, key)
    if (element === JsonNull) return null
    return json.decodeFromJsonElement<T>(element)
}

private fun buildSettings(root: JsonObject): SettingsManager = SettingsManager(
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
)

private fun stableIdsIn(text: String): Set<String> =
    STABLE_ID.findAll(text).map { it.value.lowercase() }.toSet()

private val STABLE_ID =
    Regex("custom-provider-[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", RegexOption.IGNORE_CASE)

/**
 * A provider id that was minted during this restore (random entropy) cannot be pinned to a value,
 * so it is masked — but only when it does not appear anywhere in the scenario input, which pins
 * retained/merged ids exactly and generated-vs-retained structurally.
 */
private fun maskGeneratedIds(text: String, knownIds: Set<String>): String =
    STABLE_ID.replace(text) { match ->
        if (match.value.lowercase() in knownIds) match.value else GENERATED_ID
    }

private const val GENERATED_ID = "custom-provider-<generated>"

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenarioFile = File(path)
            File(outputDir, scenarioFile.nameWithoutExtension + ".trace")
                .writeText(render(scenarioFile) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

private fun render(scenarioFile: File): String {
    val raw = scenarioFile.readText()
    val root = Json.parseToJsonElement(raw).jsonObject
    val op = root["op"]?.let { (it as? JsonPrimitive)?.contentOrNull }
        ?: error("scenario field `op` is required")
    val snapshot = root["settings"]?.jsonObject ?: error("scenario field `settings` is required")
    val settings = buildSettings(snapshot)
    val knownIds = stableIdsIn(raw)

    return when (op) {
        "export" -> {
            val customFontIncluded = (root["customFontIncluded"] as? JsonPrimitive)
                ?.contentOrNull?.toBooleanStrictOrNull() ?: false
            val exported = runBlocking {
                PortableSettingsArchive.toJsonObject(settings, customFontIncluded)
            }
            maskGeneratedIds(exported.toString(), knownIds)
        }
        "restore" -> {
            val obj = root["object"]?.jsonObject ?: error("scenario field `object` is required")
            val replace = (root["replace"] as? JsonPrimitive)?.contentOrNull?.toBooleanStrictOrNull() ?: false
            val allowLegacySecrets = (root["allowLegacySecrets"] as? JsonPrimitive)
                ?.contentOrNull?.toBooleanStrictOrNull() ?: false
            val font = root["restoredCustomFont"]?.let { element ->
                if (element === JsonNull) null else {
                    val fontObject = element.jsonObject
                    RestoredCustomFont(
                        path = fontObject["path"]?.let { (it as? JsonPrimitive)?.contentOrNull.orEmpty() }.orEmpty(),
                        displayName = fontObject["displayName"]?.let { (it as? JsonPrimitive)?.contentOrNull.orEmpty() }.orEmpty(),
                    )
                }
            }
            val promptMap = buildMap {
                (root["resolveSystemPromptIds"]?.jsonObject ?: JsonObject(emptyMap()))
                    .forEach { (key, value) ->
                        val resolved = (value as? JsonPrimitive)?.contentOrNull
                        put(key, resolved)
                    }
            }
            val fontFileExists = (root["fontFileExists"] as? JsonPrimitive)
                ?.contentOrNull?.toBooleanStrictOrNull() ?: false
            val platformFont = File(PLATFORM_FONT)
            if (fontFileExists) platformFont.writeText("platform font") else platformFont.delete()

            var failure: Throwable? = null
            var warnings: List<String> = emptyList()
            runBlocking {
                try {
                    warnings = PortableSettingsArchive.restoreFromJsonObject(
                        obj = obj,
                        sm = settings,
                        replace = replace,
                        allowLegacySecrets = allowLegacySecrets,
                        restoredCustomFont = font,
                        resolveSystemPromptId = { id -> id?.let { promptMap[it] } },
                    )
                } catch (error: Throwable) {
                    failure = error
                }
            }

            val trace = buildJsonObject {
                put("calls", buildJsonArray {
                    settings.calls.forEach { add(it) }
                })
                put("warnings", buildJsonArray {
                    warnings.forEach { add(it) }
                })
                val error = failure
                if (error != null) {
                    put("error", buildJsonObject {
                        put("type", error::class.simpleName ?: error.javaClass.name)
                        put("message", error.message ?: "")
                    })
                }
            }.toString()
            maskGeneratedIds(trace, knownIds)
        }
        else -> error("unknown op `$op`")
    }
}

private const val PLATFORM_FONT = "platform_font.ttf"
