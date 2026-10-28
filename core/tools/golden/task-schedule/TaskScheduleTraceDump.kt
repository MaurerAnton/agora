package task.schedule.golden

import com.newoether.agora.automation.ScheduleType
import com.newoether.agora.automation.TaskSchedule
import com.newoether.agora.automation.hasSchedule
import com.newoether.agora.data.local.TaskEntity
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonPrimitive
import java.io.File
import java.util.Locale
import java.util.TimeZone

/**
 * Stand driver for automation/TaskSchedule.kt, which compiles verbatim from the app tree.
 *
 * TaskSchedule.kt itself is pure -- it only imports java.util.Calendar. Its `hasSchedule()`
 * extension is declared on the Room entity TaskEntity, which lives in a file full of other
 * entities and cannot compile host-side, so agora_extract task-entity lifts that one declaration out
 * verbatim and PlatformStubs.kt supplies the two Room markers.
 *
 * Determinism: the default time zone and locale are pinned here, at the JVM level, because
 * `Calendar.getInstance()` and `SimpleDateFormat` read them. That is environment configuration,
 * not a change to the source under test, and both are recorded in the trace so the port is
 * pinned against the same settings.
 *
 * What CANNOT be pinned: `switchedTo` seeds missing fields from `Calendar.getInstance()` and
 * compares against `System.currentTimeMillis()`. A verbatim stand cannot freeze either, and the
 * JDK offers no supported way to do so. Scenarios therefore avoid that code and the boundary is
 * documented in the stand README.
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

private fun scheduleTypeOf(name: String?): ScheduleType = when (name) {
    null, "ONCE" -> ScheduleType.ONCE
    "DAILY" -> ScheduleType.DAILY
    "WEEKLY" -> ScheduleType.WEEKLY
    "MONTHLY" -> ScheduleType.MONTHLY
    "YEARLY" -> ScheduleType.YEARLY
    else -> error("unknown schedule type `$name`")
}

/** Stable, order-preserving rendering of the value object the scenarios drive. */
private fun show(schedule: TaskSchedule): String {
    val days = schedule.daysOfWeek.joinToString(",")  // a Set: iteration order is LinkedHashSet order
    return "${schedule.type},${schedule.hour},${schedule.minute},$days," +
        "${schedule.dayOfMonth},${schedule.month},${schedule.onceAtMillis}"
}

private fun parseSchedule(root: JsonObject): TaskSchedule {
    val node = root["schedule"] as? JsonObject ?: error("scenario field `schedule` is required")
    fun text(key: String): String? = (node[key] as? JsonPrimitive)?.takeIf { it.isString }?.content
    fun int(key: String, fallback: Int): Int = (node[key] as? JsonPrimitive)?.content?.toInt() ?: fallback
    fun long(key: String, fallback: Long): Long = (node[key] as? JsonPrimitive)?.content?.toLong() ?: fallback

    // toCollection(LinkedHashSet) keeps the scenario's order, matching what a LinkedHashSet does
    // in the app.
    val days: Set<Int> = (node["daysOfWeek"] as? JsonArray)
        ?.map { (it as JsonPrimitive).content.toInt() }
        ?.toCollection(LinkedHashSet())
        ?: emptySet()

    return TaskSchedule(
        type = scheduleTypeOf(text("type")),
        hour = int("hour", 0),
        minute = int("minute", 0),
        daysOfWeek = days,
        dayOfMonth = int("dayOfMonth", 1),
        month = int("month", 1),
        onceAtMillis = long("onceAtMillis", 0L),
    )
}

fun main(args: Array<String>) {
    val scenario = File(args[0])
    val root = Json.parseToJsonElement(scenario.readText()) as JsonObject

    fun text(name: String): String? {
        val element = root[name] ?: return null
        if (element !is JsonPrimitive || !element.isString) return null
        return element.content
    }
    // A JSON null parses as a JsonPrimitive whose content is "null", which toLongOrNull rejects.
    fun number(name: String): Long? = (root[name] as? JsonPrimitive)?.content?.toLongOrNull()

    TimeZone.setDefault(TimeZone.getTimeZone(text("tz") ?: "UTC"))
    Locale.setDefault(Locale.forLanguageTag(text("locale") ?: "en-US"))

    val op = text("op") ?: error("scenario field `op` is required")
    val schedule = if (root["schedule"] is JsonObject) parseSchedule(root) else null

    val trace = StringBuilder()
    trace.append("name|${esc(text("name") ?: scenario.nameWithoutExtension)}\n")
    trace.append("op|${esc(op)}\n")
    trace.append("tz|${esc(TimeZone.getDefault().id)}\n")
    trace.append("locale|${esc(Locale.getDefault().toLanguageTag())}\n")
    trace.append("schedule|${schedule?.let { show(it) } ?: "-"}\n")

    val cronExpr = text("cronExpr") ?: ""
    val runAt = number("runAt")

    when (op) {
        "toCron" -> {
            trace.append("out|string|${esc(schedule!!.toCron())}\n")
        }

        "toRunAt" -> {
            trace.append("out|long|${opt(schedule!!.toRunAt())}\n")
        }

        "parse" -> {
            trace.append("args|${esc(cronExpr)}|${opt(runAt)}\n")
            val parsed = TaskSchedule.parse(cronExpr, runAt)
            trace.append("out|schedule|${parsed?.let { show(it) } ?: "-"}\n")
        }

        "switchedTo" -> {
            val newType = scheduleTypeOf(text("newType"))
            trace.append("args|${newType}\n")
            trace.append("out|schedule|${show(schedule!!.switchedTo(newType))}\n")
        }

        "withOnceAt" -> {
            val year = number("year")!!.toInt()
            val month = number("month")!!.toInt()
            val day = number("day")!!.toInt()
            trace.append("args|$year|$month|$day\n")
            trace.append("out|schedule|${show(schedule!!.withOnceAt(year, month, day))}\n")
        }

        "withTime" -> {
            val hour = number("newHour")!!.toInt()
            val minute = number("newMinute")!!.toInt()
            trace.append("args|$hour|$minute\n")
            trace.append("out|schedule|${show(schedule!!.withTime(hour, minute))}\n")
        }

        "formatOnDate" -> {
            trace.append("out|string|${esc(schedule!!.formatOnDate())}\n")
        }

        "hasSchedule" -> {
            trace.append("args|${esc(cronExpr)}|${opt(runAt)}\n")
            val entity = TaskEntity(
                id = "t1",
                name = "task",
                prompt = "run",
                cronExpr = cronExpr,
                runAt = runAt,
                nextRunAt = 0L,
            )
            trace.append("out|bool|${if (entity.hasSchedule()) 1 else 0}\n")
        }

        else -> error("unknown op `$op`")
    }

    print(trace)
}
