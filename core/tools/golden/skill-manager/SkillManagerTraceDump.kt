// Golden trace driver for the SkillManager, used to pin the C++ port.
//
// The SkillManager and DescriptionMetadataStore compile VERBATIM from the app tree; only the two
// Android/coroutines platform types are stand-ins (see PlatformStub.kt / StateFlowStub.kt). The
// driver calls the REAL implementation — no transcription. The C++ core must reproduce every trace
// byte for byte.
//
// Scenario format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package skill.manager.golden

import android.content.Context
import com.newoether.agora.data.SkillManager
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
    val mode: String = "list",
    @SerialName("file_name") val fileName: String = "",
    @SerialName("content_param") val contentParam: String = "",
    @SerialName("setup_content") val setupContent: String = "",
    @SerialName("setup_description") val setupDescription: String = "",
    @SerialName("setup_target_name") val setupTargetName: String = "",
    @SerialName("setup_target_content") val setupTargetContent: String = "",
    @SerialName("setup_raw_name") val setupRawName: String = "",
    @SerialName("setup_raw_content") val setupRawContent: String = "",
    @SerialName("setup_raw_meta") val setupRawMeta: String = "",
    @SerialName("old_string") val oldString: String? = null,
    @SerialName("new_string") val newString: String? = null,
    @SerialName("new_name") val newName: String = "",
    val description: String = "",
    @SerialName("meta_json") val metaJson: String = "",
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: SkillManagerTraceDump <scenario.json> <tempDir>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val filesDir = File(args[1], "scenario_${scenario.name}").absoluteFile
    val skills = SkillManager(Context(filesDir))
    val skillDir = File(filesDir, "skill_db")

    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))

    // Scenarios that mutate or read an existing file pre-create it through the real manager;
    // raw setup writes cover a non-.md neighbour, a corrupt metadata file, and an invalid one.
    if (scenario.setupContent.isNotBlank()) {
        skills.createFile(scenario.fileName, scenario.setupContent, scenario.setupDescription)
    }
    if (scenario.setupTargetName.isNotBlank()) {
        skills.createFile(scenario.setupTargetName, scenario.setupTargetContent)
    }
    if (scenario.setupRawName.isNotBlank()) {
        skillDir.mkdirs()
        File(skillDir, scenario.setupRawName).writeText(scenario.setupRawContent)
    }
    if (scenario.setupRawMeta.isNotBlank()) {
        skillDir.mkdirs()
        File(skillDir, "skill_meta.json").writeText(scenario.setupRawMeta)
    }

    // Error paths (missing file, duplicate create, rename conflict, corrupt metadata, ...) are
    // part of the pinned behaviour: record the message instead of crashing so the C++ port can
    // match it.
    try {
        when (scenario.mode) {
            "description_get" -> line("description", esc(skills.getDescription(scenario.fileName)))
            "list" -> {
                val files = skills.listFiles()
                line("count", files.size.toString())
                for (f in files) line("file", esc(f.name), esc(f.description))
            }
            "catalog" -> line("catalog", esc(skills.catalog()))
            "meta" -> line("json", esc(skills.getMetaJson()))
            "meta_save" -> {
                skills.saveMetaJson(scenario.metaJson)
                line("json", esc(skills.getMetaJson()))
            }
            "read" -> line("content", esc(skills.readFile(scenario.fileName)))
            "create" -> line(
                "result",
                esc(
                    skills.createFile(
                        scenario.fileName,
                        scenario.contentParam,
                        scenario.description.takeIf { it.isNotBlank() }.orEmpty(),
                    )
                )
            )
            "edit" -> line(
                "result",
                esc(
                    skills.editFile(
                        name = scenario.fileName,
                        content = scenario.contentParam.takeIf { it.isNotBlank() },
                        newName = scenario.newName.takeIf { it.isNotBlank() },
                        description = scenario.description.takeIf { it.isNotBlank() },
                        oldString = scenario.oldString,
                        newString = scenario.newString,
                    )
                )
            )
            "delete" -> line("result", esc(skills.deleteFile(scenario.fileName)))
            "revision" -> line("catalog", skills.catalogRevision.value.toString())
            else -> error("unknown mode: ${scenario.mode}")
        }
    } catch (e: Throwable) {
        line("error", esc(e.message ?: e::class.simpleName ?: "error"))
    }
    print(out)
}
