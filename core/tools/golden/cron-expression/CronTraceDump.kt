package cron.expression.golden

import com.newoether.agora.automation.CronExpression
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.longOrNull
import java.io.File
import java.util.TimeZone

/**
 * Stand driver for automation/CronExpression.kt, which compiles VERBATIM from the app tree.
 *
 * The class is pure and depends on nothing but java.util, so the stand needs no stubs at all --
 * the rarest and cleanest kind of harness in this repository.
 *
 * Two operations are pinned:
 *
 *  - parse: whether the expression is well formed AND the exact value set each field expands to.
 *    Validity alone would hide the whole grammar, so the five sets and the two restriction flags
 *    are dumped as well. They are private in Kotlin, so the driver reads them reflectively; that
 *    is test-only introspection and does not touch the source under test.
 *  - next: the resolved instant, which is where Calendar's lenient field arithmetic and the
 *    timezone database decide the answer.
 */
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

private fun opt(value: Long?): String = if (value == null) "-" else value.toString()

private const val SET_FIELDS = "minutes|hours|daysOfMonth|months|daysOfWeek"
private const val FLAG_FIELDS = "domRestricted|dowRestricted"

private fun readField(expression: CronExpression, name: String): Any {
    val field = CronExpression::class.java.getDeclaredField(name)
    field.isAccessible = true
    return field.get(expression)!!
}

private fun dumpSets(expression: CronExpression): List<String> =
    SET_FIELDS.split('|').map { name ->
        @Suppress("UNCHECKED_CAST")
        val values = readField(expression, name) as Set<Int>
        values.joinToString(",")
    }

private fun dumpFlags(expression: CronExpression): List<Boolean> =
    FLAG_FIELDS.split('|').map { name -> readField(expression, name) as Boolean }

private fun text(value: String?): String = if (value == null) "-" else esc(value)

fun main(args: Array<String>) {
    val scenario = File(args[0])
    val root = Json.parseToJsonElement(scenario.readText()) as JsonObject

    fun text(name: String): String? {
        val element = root[name] ?: return null
        if (element !is JsonPrimitive || !element.isString) return null
        return element.content
    }

    val name = text("name") ?: scenario.nameWithoutExtension
    val expr = text("expr") ?: error("scenario field `expr` is required")
    val trace = StringBuilder()
    trace.append("name|${esc(name)}\n")
    trace.append("expr|${esc(expr)}\n")

    val parsed = CronExpression.parse(expr)
    trace.append("valid|${if (parsed != null) 1 else 0}\n")

    if (parsed != null) {
        dumpSets(parsed).forEachIndexed { index, values ->
            trace.append("${SET_FIELDS.split('|')[index]}|${esc(values)}\n")
        }
        dumpFlags(parsed).forEachIndexed { index, flag ->
            trace.append("${FLAG_FIELDS.split('|')[index]}|${if (flag) 1 else 0}\n")
        }

        if ((root["next"] as? JsonPrimitive)?.booleanOrNull == true) {
            val afterMillis = (root["after_millis"] as? JsonPrimitive)?.longOrNull
                ?: error("scenario field `after_millis` is required for op=next")
            // TimeZone.getTimeZone silently falls back to GMT for an unknown id, which is pinned
            // rather than treated as an error.
            val zone = TimeZone.getTimeZone(text("zone") ?: "UTC")
            trace.append("zone|${esc(zone.id)}\n")
            trace.append("next|${opt(parsed.next(afterMillis, zone))}\n")
        }
    }

    print(trace)
}