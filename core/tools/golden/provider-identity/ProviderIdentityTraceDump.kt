// Golden trace driver for the custom provider identity policy, used to pin the C++ port.
//
// The identity half of CustomProviderIdentity.kt is extracted verbatim (`agora_extract provider-identity`) and
// CustomProviderConfig.kt / CustomProviderNamePolicy.kt compile verbatim from the app tree, so the
// driver calls the real code — no transcription. normalize() takes its id generator as a
// parameter, which keeps every scenario deterministic.
//
// Scenario format: see README.md in this directory. Build/run: see run_golden.sh.
package provider.identity.golden

import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.CustomProviderIdentityPolicy
import com.newoether.agora.data.canonicalCustomModelId
import com.newoether.agora.data.remapModelPreferenceKeys
import com.newoether.agora.data.remapProviderReference
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

/** Field-safe escaping for the trace format: no raw separators or control characters survive. */
private fun esc(value: String): String = buildString {
    for (c in value) {
        when (c) {
            '\\' -> append("\\\\")
            '|' -> append("\\p")
            ';' -> append("\\s")
            '\n' -> append("\\n")
            '\r' -> append("\\r")
            '\t' -> append("\\t")
            else -> append(c)
        }
    }
}

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "legacyId",
    val providerName: String = "",
    val text: String = "",
    val replacement: String = "PROVIDER",
    val modelId: String = "",
    val aliases: Map<String, String> = emptyMap(),
    val migrations: Map<String, String> = emptyMap(),
    val providers: List<ScenarioProvider> = emptyList(),
    val occupiedIds: List<String> = emptyList(),
    val newIds: List<String> = emptyList(),
)

@Serializable
private data class ScenarioProvider(
    val name: String,
    val id: String = "",
    val legacyNames: List<String> = emptyList(),
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ProviderIdentityTraceDump <scenario.json> <unusedRoot>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    when (scenario.mode) {
        "legacyId" -> line("legacyId", CustomProviderIdentityPolicy.legacyId(scenario.providerName))
        "replaceIds" -> line(
            "text",
            esc(
                CustomProviderIdentityPolicy.replaceStableIdsForDisplay(scenario.text) {
                    scenario.replacement
                },
            ),
        )
        "remap" -> line("remap", esc(scenario.modelId.remapProviderReference(scenario.migrations)))
        "canonical" -> line(
            "model",
            esc(
                canonicalCustomModelId(
                    scenario.modelId,
                    scenario.providers.map { CustomProviderConfig(it.name, id = it.id, legacyNames = it.legacyNames.toSet()) },
                ),
            ),
        )
        "preferenceKeys" -> {
            val result = remapModelPreferenceKeys(scenario.aliases, scenario.migrations)
            line("count", result.size.toString())
            for ((key, value) in result) line("key", esc(key), esc(value))
        }
        "normalize" -> {
            var next = 0
            val result = CustomProviderIdentityPolicy.normalize(
                rawProviders = scenario.providers.map {
                    CustomProviderConfig(it.name, id = it.id, legacyNames = it.legacyNames.toSet())
                },
                occupiedIds = scenario.occupiedIds.toSet(),
                idGenerator = { scenario.newIds.getOrElse(next++) { "custom-provider-00000000-0000-4000-8000-000000000000" } },
            )
            line("providers", result.providers.size.toString())
            for ((index, provider) in result.providers.withIndex()) {
                line(
                    "provider",
                    index.toString(),
                    esc(provider.name),
                    esc(provider.id),
                    esc(provider.legacyNames.joinToString(";")),
                )
            }
            line("migrations", result.migrations.size.toString())
            for ((index, migration) in result.migrations.withIndex()) {
                line("migration", index.toString(), esc(migration.legacyReference), esc(migration.providerId))
            }
        }
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}