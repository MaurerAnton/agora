// Golden trace driver for ShellDurableJobExecutor.kt.
//
// ShellDurableJobExecutor.kt and ShellToolJson.kt compile VERBATIM from the app tree. The stand
// supplies only leaf doubles: GenerationContext/ShellDeviceConfig/ToolCallData (the fields the
// executor reads) and a scripted ConchBackend standing in for the OkHttp transport. The trace
// pins the terminal job-state rules, the Conch output snapshot/cursor byte arithmetic, both
// wait/poll loops (virtual clock for waitForShellJobPolling, the real clock for the public
// paths), the exact JSON every branch builds, the backend event order and the terminal job
// acknowledgements derived from committed tool results.
package shell.durable.jobs.golden

import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.model.ToolCallData
import com.newoether.agora.tool.BackendEvent
import com.newoether.agora.tool.BackendScript
import com.newoether.agora.tool.ConchBackend
import com.newoether.agora.tool.ConchJobOutputCursor
import com.newoether.agora.tool.ScriptStep
import com.newoether.agora.tool.ShellDurableJobExecutor
import com.newoether.agora.tool.ShellJobPoller
import com.newoether.agora.tool.conchJobOutputSnapshot
import com.newoether.agora.tool.conchServerNotFoundMessage
import com.newoether.agora.tool.getConchBackend
import com.newoether.agora.tool.resolveConchDevice
import com.newoether.agora.tool.terminalShellJobAcknowledgements
import com.newoether.agora.viewmodel.GenerationContext
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext
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
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
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
    val scenario = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = scenario.string("op")
    val outcome = when (op) {
        "resolve_conch_device" -> buildJsonObject {
            val device = resolveConchDevice(scenario.string("server"), contextOf(scenario))
            if (device == null) put("device", JsonNull)
            else put("device", buildJsonObject {
                put("name", device.name)
                put("type", device.type)
            })
        }

        "conch_not_found" -> buildJsonObject {
            put("message", conchServerNotFoundMessage(scenario.string("server"), contextOf(scenario)))
        }

        "is_terminal_payload" -> buildJsonObject {
            put("terminal", ShellDurableJobExecutor().isTerminalJobPayload(scenario.string("raw")))
        }

        "max_wait_ms" -> buildJsonObject {
            put("max_wait_ms", ShellDurableJobExecutor.maxWaitMs(contextOf(scenario)))
        }

        "output_snapshot" -> snapshotOutcome(scenario)
        "output_cursor" -> cursorOutcome(scenario)
        "wait_for_job_polling" -> pollingOutcome(scenario)
        "wait_for_job" -> waitOutcome(scenario)
        "durable_foreground" -> durableOutcome(scenario)
        "list_jobs" -> toolOutcome(scenario, list = true)
        "get_jobs" -> toolOutcome(scenario, list = false)
        "terminal_acks" -> acksOutcome(scenario)
        else -> error("unknown op $op")
    }
    return buildJsonObject {
        put("op", op)
        put("scenario", scenario)
        put("outcome", outcome)
    }.toString()
}

// ---- Context / script plumbing -----------------------------------------------------------------

private fun devicesOf(scenario: JsonObject): List<ShellDeviceConfig> =
    (scenario["devices"] as? JsonArray)?.map { element ->
        val obj = element.jsonObject
        ShellDeviceConfig(name = obj.string("name"), type = obj.string("type"))
    } ?: emptyList()

private fun contextOf(scenario: JsonObject): GenerationContext = GenerationContext(
    shellDevices = devicesOf(scenario),
    toolTimeoutMs = scenario.optionalLong("tool_timeout_ms") ?: 60_000L,
)

private fun installScript(scenario: JsonObject) {
    ConchBackend.script = BackendScript(
        start = singleStep(scenario, "start"),
        poll = arraySteps(scenario, "polls"),
        list = singleStep(scenario, "list"),
        stop = if (scenario.containsKey("stop")) singleStep(scenario, "stop")
               else listOf(ScriptStep.Emit("{}")),
        startRepeat = repeatStep(scenario, "start_repeat"),
        pollRepeat = repeatStep(scenario, "poll_repeat"),
        listRepeat = repeatStep(scenario, "list_repeat"),
        stopRepeat = repeatStep(scenario, "stop_repeat"),
    )
}

private fun singleStep(scenario: JsonObject, key: String): List<ScriptStep> =
    (scenario[key] as? JsonObject)?.let { listOf(parseStep(it)) } ?: emptyList()

private fun arraySteps(scenario: JsonObject, key: String): List<ScriptStep> =
    (scenario[key] as? JsonArray)?.map { parseStep(it.jsonObject) } ?: emptyList()

private fun repeatStep(scenario: JsonObject, key: String): ScriptStep? =
    (scenario[key] as? JsonObject)?.let { parseStep(it) }

private fun parseStep(obj: JsonObject): ScriptStep = when {
    "ok" in obj -> ScriptStep.Emit(obj.string("ok"))
    "throw" in obj -> ScriptStep.Fail(obj.string("throw"))
    "cancel" in obj -> ScriptStep.CancelEmit(obj.string("cancel"))
    "cancel_throw" in obj -> ScriptStep.CancelFail(obj.string("cancel_throw"))
    else -> error("step needs ok/throw/cancel/cancel_throw: $obj")
}

private fun eventsJson(): JsonArray = buildJsonArray {
    ConchBackend.script.events.forEach { event ->
        add(
            when (event) {
                is BackendEvent.Start -> buildJsonObject {
                    put("op", "start")
                    put("command", event.command)
                    put("workdir", event.workdir)
                    put("timeout_ms", event.timeoutMs)
                }

                is BackendEvent.Get -> buildJsonObject {
                    put("op", "get")
                    put("job_id", event.jobId)
                }

                is BackendEvent.ListJobs -> buildJsonObject { put("op", "list") }

                is BackendEvent.Stop -> buildJsonObject {
                    put("op", "stop")
                    put("job_id", event.jobId)
                    if (event.timeoutMs == null) put("timeout_ms", JsonNull)
                    else put("timeout_ms", event.timeoutMs)
                }

                is BackendEvent.Close -> buildJsonObject { put("op", "close") }
            },
        )
    }
}

/**
 * The real-clock paths embed a genuine elapsed wall-clock value; the trace masks it so the frozen
 * traces stay byte-stable (the loops' structure and messages stay pinned).
 */
private fun maskWaitedMs(text: String): String =
    Regex("\"waited_ms\":\\d+").replace(text) { "\"waited_ms\":MASKED" }

// ---- Coroutine harness -------------------------------------------------------------------------

private class OpRun(val result: String?, val cancelled: Boolean)

/**
 * Runs a suspending executor call in an independent coroutine so a scripted cancellation is
 * observable as either a normal return (the loop's own timeout branch won first) or a
 * CancellationException, without cancelling the driver itself.
 */
private fun runOp(block: suspend () -> String): OpRun = runBlocking {
    var captured: String? = null
    var wasCancelled = false
    val scope = CoroutineScope(Job())
    val deferred = scope.async(Dispatchers.Unconfined) {
        try {
            captured = block()
        } catch (cancelled: CancellationException) {
            wasCancelled = true
            throw cancelled
        }
    }
    deferred.join()
    if (!wasCancelled && captured == null) {
        // Surface unexpected failures (a clean cancellation is not one).
        deferred.await()
    }
    OpRun(captured, wasCancelled)
}

// ---- Ops ----------------------------------------------------------------------------------------

private fun snapshotOutcome(scenario: JsonObject): JsonElement {
    val snapshot = conchJobOutputSnapshot(scenario.string("raw"))
    return buildJsonObject {
        if (snapshot == null) put("snapshot", JsonNull)
        else put("snapshot", buildJsonObject {
            put("text", snapshot.text)
            put("output_bytes", snapshot.outputBytes)
            put("truncated", snapshot.truncated)
        })
    }
}

private fun cursorOutcome(scenario: JsonObject): JsonElement {
    val cursor = ConchJobOutputCursor()
    return buildJsonObject {
        put("updates", buildJsonArray {
            scenario.strings("payloads").forEach { payload ->
                val update = cursor.consume(payload)
                add(buildJsonObject {
                    put("delta", update.delta)
                    put("lost_bytes", update.lostBytes)
                })
            }
        })
    }
}

private fun pollingOutcome(scenario: JsonObject): JsonElement {
    installScript(scenario)
    var clock = scenario.long("start_clock")
    var delayCount = 0
    val cancelAfterDelay = scenario.optionalInt("cancel_after_delay")
    val snapshots = mutableListOf<String>()
    val run = runOp {
        ShellDurableJobExecutor().waitForShellJobPolling(
            jobId = scenario.string("job_id"),
            serverName = scenario.string("server"),
            timeoutMs = scenario.int("timeout_ms"),
            clampedFrom = scenario.optionalInt("clamped_from"),
            ceilingMs = scenario.int("ceiling_ms"),
            poller = ShellJobPoller { ConchBackend.script.nextRun("poll") },
            onOutputSnapshot = { snapshots += it },
            nowMs = { clock },
            delayMs = { requested ->
                clock += requested
                delayCount += 1
                if (cancelAfterDelay != null && delayCount == cancelAfterDelay) {
                    currentCoroutineContext().cancel()
                }
            },
        )
    }
    return buildJsonObject {
        if (run.result == null) put("result", JsonNull) else put("result", run.result)
        put("snapshots", buildJsonArray { snapshots.forEach { add(it) } })
        put("clock", clock)
    }
}

private fun waitOutcome(scenario: JsonObject): JsonElement {
    installScript(scenario)
    val ctx = contextOf(scenario)
    val snapshots = mutableListOf<String>()
    val run = runOp {
        ShellDurableJobExecutor().waitForShellJob(scenario.string("arguments"), ctx) {
            snapshots += it
        }
    }
    return buildJsonObject {
        if (run.result == null) put("result", JsonNull)
        else put("result", maskWaitedMs(run.result))
        put("snapshots", buildJsonArray { snapshots.forEach { add(it) } })
    }
}

private fun durableOutcome(scenario: JsonObject): JsonElement {
    installScript(scenario)
    val ctx = contextOf(scenario)
    val serverName = scenario.string("server")
    val backend = getConchBackend(serverName, ctx)
        ?: error("durable_foreground scenario must resolve a Conch device (server=$serverName)")
    val outputs = mutableListOf<String>()
    val run = runOp {
        ShellDurableJobExecutor().executeDurableForeground(
            backend,
            scenario.string("command"),
            scenario.string("workdir"),
            scenario.int("wait_ms"),
        ) { outputs += it }
    }
    return buildJsonObject {
        if (run.result == null) put("result", JsonNull)
        else put("result", maskWaitedMs(run.result))
        put("cancelled", run.cancelled)
        put("outputs", buildJsonArray { outputs.forEach { add(it) } })
        put("events", eventsJson())
    }
}

private fun toolOutcome(scenario: JsonObject, list: Boolean): JsonElement {
    installScript(scenario)
    val ctx = contextOf(scenario)
    val run = runOp {
        val executor = ShellDurableJobExecutor()
        if (list) executor.listShellJobs(scenario.string("arguments"), ctx)
        else executor.getShellJob(scenario.string("arguments"), ctx)
    }
    return buildJsonObject {
        if (run.result == null) put("result", JsonNull) else put("result", run.result)
        put("events", eventsJson())
    }
}

private fun acksOutcome(scenario: JsonObject): JsonElement {
    val calls = (scenario["calls"] as? JsonArray ?: error("missing calls")).map { element ->
        val obj = element.jsonObject
        ToolCallData(
            toolName = obj.string("tool_name"),
            arguments = obj.string("arguments"),
            result = obj.string("result"),
        )
    }
    val acks = terminalShellJobAcknowledgements(calls)
    return buildJsonObject {
        put("acks", buildJsonArray {
            acks.forEach { ack ->
                add(buildJsonObject {
                    put("server_name", ack.serverName)
                    put("job_id", ack.jobId)
                })
            }
        })
    }
}

// ---- JSON helpers -------------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalLong(name: String): Long? =
    (this[name] as? JsonPrimitive)?.takeUnless { it == JsonNull }?.content?.toLongOrNull()

private fun JsonObject.optionalInt(name: String): Int? =
    (this[name] as? JsonPrimitive)?.takeUnless { it == JsonNull }?.content?.toIntOrNull()

private fun JsonObject.strings(name: String): List<String> {
    val array = this[name] as? JsonArray ?: error("missing array $name")
    return array.map { (it as JsonPrimitive).content }
}
