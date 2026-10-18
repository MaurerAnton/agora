// Golden trace driver for the MemoryManager, used to pin the C++ port.
//
// The MemoryManager and DescriptionMetadataStore compile VERBATIM from the app tree; only the two
// Android/coroutines platform types are stand-ins (see PlatformStub.kt / StateFlowStub.kt). The
// driver calls the REAL implementation — no transcription. The C++ core must reproduce every trace
// byte for byte.
//
// Scenario format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package memory.manager.golden

import android.content.Context
import com.newoether.agora.data.MemoryManager
import kotlinx.serialization.SerialName
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
    val mode: String = "active",
    @SerialName("mode_param") val modeParam: String = "replace",
    @SerialName("content_param") val contentParam: String = "",
    @SerialName("file_name") val fileName: String = "",
    @SerialName("setup_content") val setupContent: String = "",
    @SerialName("old_string") val oldString: String? = null,
    @SerialName("new_string") val newString: String? = null,
    @SerialName("new_name") val newName: String = "",
    val description: String = "",
    @SerialName("meta_json") val metaJson: String = "",
    @SerialName("setup_target_name") val setupTargetName: String = "",
    @SerialName("setup_target_content") val setupTargetContent: String = "",
    @SerialName("active_seed") val activeSeed: String = "",
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: MemoryManagerTraceDump <scenario.json> <tempDir>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val filesDir = File(args[1], "scenario_${scenario.name}").absoluteFile
    val mem = MemoryManager(Context(filesDir))

    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))

    // Scenarios that mutate an existing file pre-create it via setup_content / setup_target_*.
    if (scenario.setupContent.isNotBlank()) {
        mem.createFile(scenario.fileName, scenario.setupContent)
    }
    if (scenario.setupTargetName.isNotBlank()) {
        mem.createFile(scenario.setupTargetName, scenario.setupTargetContent)
    }
    if (scenario.activeSeed.isNotBlank()) {
        mem.updateActiveMemory(scenario.activeSeed)
    }

    // Error paths (missing file, ambiguous patch, rename conflict, ...) are part of the pinned
    // behaviour: record the message instead of crashing so the C++ port can match it.
    try {
        when (scenario.mode) {
            "active" -> {
                line(
                    "result",
                    esc(
                        mem.updateActiveMemory(
                            content = scenario.contentParam,
                            mode = scenario.modeParam,
                            oldString = scenario.oldString,
                            newString = scenario.newString,
                        )
                    )
                )
                line("active", esc(mem.getActiveMemory()))
            }
            "description_get" -> line("description", esc(mem.getDescription(scenario.fileName)))
            "description_set" -> {
                mem.setDescription(scenario.fileName, scenario.description)
                line("ok")
            }
            "list" -> {
                val files = mem.listFiles()
                line("count", files.size.toString())
                for (f in files) line("file", esc(f.name), esc(f.description))
            }
            "meta" -> line("json", esc(mem.getMetaJson()))
            "meta_save" -> {
                mem.saveMetaJson(scenario.metaJson)
                line("json", esc(mem.getMetaJson()))
            }
            "read" -> line("content", esc(mem.readFile(scenario.fileName)))
            "create" -> line(
                "result",
                esc(
                    mem.createFile(
                        scenario.fileName,
                        scenario.contentParam,
                        scenario.description.takeIf { it.isNotBlank() }.orEmpty(),
                    )
                )
            )
            "edit" -> line(
                "result",
                esc(
                    mem.editFile(
                        name = scenario.fileName,
                        content = scenario.contentParam.takeIf { it.isNotBlank() },
                        newName = scenario.newName.takeIf { it.isNotBlank() },
                        description = scenario.description.takeIf { it.isNotBlank() },
                        oldString = scenario.oldString,
                        newString = scenario.newString,
                    )
                )
            )
            "delete" -> line("result", esc(mem.deleteFile(scenario.fileName)))
            "revision" -> {
                line("active", mem.activeMemoryRevision.value.toString())
                line("catalog", mem.catalogRevision.value.toString())
            }
            else -> error("unknown mode: ${scenario.mode}")
        }
    } catch (e: Throwable) {
        line("error", esc(e.message ?: e::class.simpleName ?: "error"))
    }
    print(out)
}
