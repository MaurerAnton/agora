package com.newoether.agora.tool

import com.newoether.agora.data.ShellDeviceConfig
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.cancel
import kotlinx.coroutines.currentCoroutineContext

/**
 * Stand-local scripted double for the real ConchBackend (ShellToolBackends.kt), which owns the
 * OkHttp streaming transport. ShellDurableJobExecutor.kt compiles verbatim against this shape:
 * device, startJob, getJob, listJobs, stopJob and close. Responses come from the scenario script,
 * every call is recorded as a BackendEvent so the trace can pin the backend interaction, and a
 * "cancel" step cancels the calling coroutine exactly like a generation Stop would.
 */

internal sealed class ScriptStep {
    data class Emit(val payload: String) : ScriptStep()
    data class Fail(val message: String) : ScriptStep()
    data class CancelEmit(val payload: String) : ScriptStep()
    data class CancelFail(val message: String) : ScriptStep()
}

internal sealed class BackendEvent {
    data class Start(val command: String, val workdir: String, val timeoutMs: Int) : BackendEvent()
    data class Get(val jobId: String) : BackendEvent()
    data object ListJobs : BackendEvent()
    data class Stop(val jobId: String, val timeoutMs: Long?) : BackendEvent()
    data object Close : BackendEvent()
}

internal class BackendScript(
    private val start: List<ScriptStep>,
    private val poll: List<ScriptStep>,
    private val list: List<ScriptStep>,
    private val stop: List<ScriptStep>,
    private val startRepeat: ScriptStep?,
    private val pollRepeat: ScriptStep?,
    private val listRepeat: ScriptStep?,
    private val stopRepeat: ScriptStep?,
) {
    val events = mutableListOf<BackendEvent>()
    private val cursors = mutableMapOf<String, Int>()

    fun next(kind: String): ScriptStep {
        val steps = when (kind) {
            "start" -> start
            "poll" -> poll
            "list" -> list
            "stop" -> stop
            else -> error("unknown script kind $kind")
        }
        val repeat = when (kind) {
            "start" -> startRepeat
            "poll" -> pollRepeat
            "list" -> listRepeat
            "stop" -> stopRepeat
            else -> error("unreachable")
        }
        val cursor = cursors.getOrPut(kind) { 0 }
        val step = if (cursor < steps.size) {
            steps[cursor]
        } else {
            repeat ?: ScriptStep.Fail("$kind script exhausted")
        }
        cursors[kind] = cursor + 1
        return step
    }

    suspend fun run(step: ScriptStep): String = when (step) {
        is ScriptStep.Emit -> step.payload
        is ScriptStep.Fail -> throw Exception(step.message)
        is ScriptStep.CancelEmit -> {
            currentCoroutineContext().cancel()
            step.payload
        }
        is ScriptStep.CancelFail -> {
            currentCoroutineContext().cancel()
            throw CancellationException(step.message)
        }
    }

    suspend fun nextRun(kind: String): String = run(next(kind))
}

internal class ConchBackend(val device: ShellDeviceConfig) {
    companion object {
        var script: BackendScript = BackendScript(
            start = emptyList(),
            poll = emptyList(),
            list = emptyList(),
            stop = emptyList(),
            startRepeat = null,
            pollRepeat = null,
            listRepeat = null,
            stopRepeat = null,
        )
    }

    suspend fun startJob(cmd: String, workdir: String, timeoutMs: Int): String {
        script.events += BackendEvent.Start(cmd, workdir, timeoutMs)
        return script.nextRun("start")
    }

    suspend fun getJob(jobId: String): String {
        script.events += BackendEvent.Get(jobId)
        return script.nextRun("poll")
    }

    suspend fun listJobs(): String {
        script.events += BackendEvent.ListJobs
        return script.nextRun("list")
    }

    suspend fun stopJob(jobId: String, callTimeoutMillis: Long? = null): String {
        script.events += BackendEvent.Stop(jobId, callTimeoutMillis)
        return script.nextRun("stop")
    }

    fun close() {
        script.events += BackendEvent.Close
    }
}
