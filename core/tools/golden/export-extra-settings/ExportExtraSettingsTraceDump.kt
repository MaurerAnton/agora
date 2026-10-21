// Golden trace driver for data/ExportExtraSettings.kt, compiled verbatim from the app tree.
//
// A scenario supplies the legacy JsonObject, the replace/allowSecrets/allowedConversationIds
// arguments, and the seeded modelAliases/customModels flows. The trace records, in call order,
// every SettingsManager save the policy performs and the exact argument it passes. Floats are
// rendered with Float.toString() as strings, so the trace also pins values such as 0.1f.
package export.extra.settings.golden

import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.ExportExtraSettings
import com.newoether.agora.data.RecordedCall
import com.newoether.agora.data.SavedBoolean
import com.newoether.agora.data.SavedConversation
import com.newoether.agora.data.SavedFloat
import com.newoether.agora.data.SavedInt
import com.newoether.agora.data.SavedString
import com.newoether.agora.data.SavedStringMap
import com.newoether.agora.data.SavedStringSet
import com.newoether.agora.data.SavedValue
import com.newoether.agora.data.SettingsManager
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonObjectBuilder
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.File

private val json = Json

private fun JsonObject.arrayOrEmpty(name: String): JsonArray =
    (this[name] as? JsonArray) ?: JsonArray(emptyList())

private fun JsonObject.objectOrEmpty(name: String): JsonObject =
    (this[name] as? JsonObject) ?: JsonObject(emptyMap())

private fun JsonObject.flag(name: String, default: Boolean): Boolean =
    (this[name] as? JsonPrimitive)?.contentOrNull?.toBooleanStrictOrNull() ?: default

private fun JsonObject.stringMap(name: String): Map<String, String> =
    objectOrEmpty(name).mapValues { (_, value) -> value.jsonPrimitive.content }

private fun JsonObject.stringSet(name: String): Set<String> =
    arrayOrEmpty(name).mapTo(linkedSetOf()) { it.jsonPrimitive.content }

private fun JsonObjectBuilder.putStrings(values: Collection<String>) {
    put("strings", buildJsonArray { values.forEach { add(it) } })
}

private fun JsonObjectBuilder.putStringMap(values: Map<String, String>) {
    put("map", buildJsonObject { values.forEach { (key, value) -> put(key, value) } })
}

private fun ConversationSettings.toJson(): JsonObject = buildJsonObject {
    contextWindow?.let { put("contextWindow", it) }
    temperature?.let { put("temperature", it.toString()) }
    maxTokens?.let { put("maxTokens", it) }
    topP?.let { put("topP", it.toString()) }
    frequencyPenalty?.let { put("frequencyPenalty", it.toString()) }
    presencePenalty?.let { put("presencePenalty", it.toString()) }
    codeExecutionEnabled?.let { put("codeExecutionEnabled", it) }
    googleSearchEnabled?.let { put("googleSearchEnabled", it) }
    openAiWebSearchEnabled?.let { put("openAiWebSearchEnabled", it) }
    thinkingEnabled?.let { put("thinkingEnabled", it) }
    thinkingLevel?.let { put("thinkingLevel", it) }
    thinkingBudgetEnabled?.let { put("thinkingBudgetEnabled", it) }
    thinkingBudgetTokens?.let { put("thinkingBudgetTokens", it) }
    openAiServiceTierEnabled?.let { put("openAiServiceTierEnabled", it) }
    openAiServiceTier?.let { put("openAiServiceTier", it) }
    webSearchEnabled?.let { put("webSearchEnabled", it) }
    shellEnabled?.let { put("shellEnabled", it) }
    lowContextModeEnabled?.let { put("lowContextModeEnabled", it) }
}

private fun SavedValue.toJson(): JsonObject = when (this) {
    is SavedBoolean -> buildJsonObject { put("bool", value) }
    is SavedInt -> buildJsonObject { put("int", value) }
    is SavedFloat -> buildJsonObject { put("float", value.toString()) }
    is SavedString -> buildJsonObject { put("string", value) }
    is SavedStringSet -> buildJsonObject { putStrings(value) }
    is SavedStringMap -> buildJsonObject { putStringMap(value) }
    is SavedConversation -> buildJsonObject {
        put("conversationId", conversationId)
        put("settings", settings.toJson())
    }
}

private fun RecordedCall.toJson(): JsonObject = buildJsonObject {
    put("name", name)
    put("value", value.toJson())
}

private fun traceJson(calls: List<RecordedCall>, failure: Throwable?): JsonObject = buildJsonObject {
    put("calls", buildJsonArray {
        calls.forEach { add(it.toJson()) }
    })
    // Only the exception category is part of the settings contract; messages are kotlinx/JVM
    // diagnostics and are intentionally not pinned.
    if (failure != null) {
        put("error", failure::class.simpleName ?: failure.javaClass.name)
    }
}

private fun render(scenarioFile: File): String {
    val root = Json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val obj = root["object"]?.jsonObject ?: error("scenario field `object` is required")
    val replace = root.flag("replace", false)
    val allowSecrets = root.flag("allowSecrets", false)
    val allowedConversationIds = root.stringSet("allowedConversationIds")
    val settings = SettingsManager(
        modelAliasesSeed = root.stringMap("modelAliases"),
        customModelsSeed = root.stringSet("customModels"),
    )

    var failure: Throwable? = null
    runBlocking {
        try {
            ExportExtraSettings.restoreLegacyFromJsonObject(
                obj = obj,
                sm = settings,
                replace = replace,
                allowSecrets = allowSecrets,
                allowedConversationIds = allowedConversationIds,
            )
        } catch (error: Throwable) {
            failure = error
        }
    }

    return json.encodeToString(traceJson(settings.calls.toList(), failure))
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
