// Golden trace driver for automation/LoopPolicy.kt, which compiles VERBATIM from the app tree.
//
// LoopPolicy has no imports at all -- it is constants plus five pure functions -- so this stand
// needs no stubs and no extraction. The constants themselves are part of the contract (they are
// what the scheduler and the model-facing tools agree on), so every trace records them.
package loop.policy.golden

import com.newoether.agora.automation.LoopPolicy
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import java.io.File

/** Field-safe escaping: no raw separators or control characters survive. */
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

private fun opt(value: String?): String = value?.let(::esc) ?: "-"

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject

    fun text(name: String): String? =
        (root[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

    // A JSON number, including a negative one. A JSON null parses as a primitive whose content is
    // "null", which toLongOrNull rejects.
    fun number(name: String): Long? = (root[name] as? JsonPrimitive)?.content?.toLongOrNull()

    val op = text("op") ?: error("scenario field `op` is required")

    val out = StringBuilder()
    out.append("name|").append(esc(text("name") ?: "")).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("constants|")
        .append(LoopPolicy.MIN_INTERVAL_SECONDS).append('|')
        .append(LoopPolicy.MAX_INTERVAL_SECONDS).append('|')
        .append(LoopPolicy.MIN_INTERVAL_MS).append('|')
        .append(LoopPolicy.MAX_INTERVAL_MS).append('|')
        .append(LoopPolicy.DEFAULT_MAX_CYCLES).append('|')
        .append(LoopPolicy.MIN_MAX_CYCLES).append('|')
        .append(LoopPolicy.MAX_MAX_CYCLES).append('|')
        .append(esc(LoopPolicy.DEFAULT_PROMPT)).append('\n')

    when (op) {
        "validate" -> {
            val intervalMs = number("intervalMs") ?: error("`intervalMs` is required")
            val maxCycles = number("maxCycles")?.toInt() ?: error("`maxCycles` is required")
            out.append("args|").append(intervalMs).append('|').append(maxCycles).append('\n')
            out.append("out|string?|").append(opt(LoopPolicy.validate(intervalMs, maxCycles))).append('\n')
        }

        "normalizePrompt" -> {
            val prompt = text("prompt")
            out.append("args|").append(opt(prompt)).append('\n')
            out.append("out|string?|").append(opt(LoopPolicy.normalizePrompt(prompt))).append('\n')
        }

        "promptForExecution" -> {
            val prompt = text("prompt")
            out.append("args|").append(opt(prompt)).append('\n')
            out.append("out|string|").append(esc(LoopPolicy.promptForExecution(prompt))).append('\n')
        }

        "nextFireAt" -> {
            val now = number("now") ?: error("`now` is required")
            val intervalMs = number("intervalMs") ?: error("`intervalMs` is required")
            out.append("args|").append(now).append('|').append(intervalMs).append('\n')
            out.append("out|long|").append(LoopPolicy.nextFireAt(now, intervalMs)).append('\n')
        }

        "nextRevision" -> {
            val current = number("current") ?: error("`current` is required")
            out.append("args|").append(current).append('\n')
            out.append("out|long|").append(LoopPolicy.nextRevision(current)).append('\n')
        }

        else -> error("unknown op `$op`")
    }

    print(out)
}
