// Golden trace driver for data/PortableSettingsReset.kt (compiled verbatim from the app tree).
//
// The driver seeds an in-memory preference store with every key of the settings schema, runs the
// real reset policy and prints the surviving keys (the ones the reset deliberately keeps) plus
// the two containers the policy rewrites. That pins the portable-allowlist boundary exactly.
package portable.settings.reset.golden

import androidx.datastore.preferences.core.InMemoryPreferences
import com.newoether.agora.data.clearPortableSettings
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

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
    val schemaKeys = (root["schemaKeys"] as? kotlinx.serialization.json.JsonArray)?.map {
        (it as kotlinx.serialization.json.JsonPrimitive).content
    } ?: SCHEMA_KEY_NAMES
    val preferences = InMemoryPreferences()
    for (key in schemaKeys) preferences.put(key, "value:$key")

    clearPortableSettings(preferences)

    val trace = buildJsonObject {
        put("surviving", buildJsonArray { preferences.names.forEach { add(it) } })
        put("removed", buildJsonArray {
            schemaKeys.filterNot { it in preferences.names }.forEach { add(it) }
        })
        put("removedInOrder", buildJsonArray { preferences.removals.forEach { add(it) } })
        put("written", buildJsonArray {
            preferences.writes.forEach { (key, value) ->
                add(buildJsonObject {
                    put("key", key)
                    put("value", value)
                })
            }
        })
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
