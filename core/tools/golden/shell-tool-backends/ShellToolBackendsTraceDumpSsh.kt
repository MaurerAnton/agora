// SSH section of the golden trace driver for tool/ShellToolBackends.kt.
//
// Split out of ShellToolBackendsTraceDump.kt to satisfy the 800-line Kotlin source
// budget. The section is self-contained: the JSch subset script, the event log and the
// SFTP flows. Scenario helpers (string/int/attempt/device/...) stay in the main driver
// file as internal, shared with the Conch and Sandbox sections.
package shell.tool.backends.golden


import com.jcraft.jsch.JschEvent
import com.jcraft.jsch.JschScript
import com.newoether.agora.tool.SshBackend
import com.newoether.agora.tool.ToolExecutionEvent
import com.newoether.agora.util.SshClient
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put


// ---- SshBackend -------------------------------------------------------------------------------------

private fun installJschScript(scenario: JsonObject) {
    JschScript.reset()
    val script = scenario["jsch"] as? JsonObject ?: return
    script.optionalString("presented_key")?.let {
        JschScript.presentedKey = it.toByteArray(Charsets.UTF_8)
    }
    script.optionalString("strict_rejection")?.let { JschScript.strictRejectionMessage = it }
    script.optionalString("connect_failure")?.let { JschScript.connectFailure = it }
    script.optionalString("open_channel_failure")?.let { JschScript.openChannelFailure = it }
    script.optionalLong("exit_status")?.let { JschScript.exitStatus = it.toInt() }
    script.optionalBoolean("never_close")?.let { JschScript.neverClose = it }
    script.optionalString("stdout")?.let { JschScript.stdout = it.toByteArray(Charsets.UTF_8) }
    script.optionalString("stderr")?.let { JschScript.stderr = it.toByteArray(Charsets.UTF_8) }
    script.optionalString("pwd")?.let { JschScript.pwdPath = it }

    val statsObj = script["stats"] as? JsonObject
    if (statsObj != null) {
        JschScript.stats = statsObj.entries.associate { (path, value) ->
            val obj = value as JsonObject
            path to com.jcraft.jsch.StatScript(
                isDir = obj.optionalBoolean("is_dir") ?: false,
                size = obj.optionalLong("size") ?: 0,
            )
        }
    }
    (script["mkdir_failures"] as? JsonArray)?.let { array ->
        JschScript.mkdirFailures = array.map { (it as JsonPrimitive).content }.toSet()
    }
    val putFailures = script["put_failures"] as? JsonObject
    if (putFailures != null) {
        JschScript.putFailures = putFailures.entries.associate { (path, value) ->
            path to (value as JsonPrimitive).content
        }
    }
    val filesObj = script["file_bytes"] as? JsonObject
    if (filesObj != null) {
        JschScript.fileBytes = filesObj.entries.associate { (path, value) ->
            path to (value as JsonPrimitive).content.toByteArray(Charsets.UTF_8)
        }
    }
    // file_bytes_second: a second read of the same path returns these bytes (edit's
    // readEditableFile runs twice; the second must differ to pin the concurrent guard).
    val secondObj = script["file_bytes_second"] as? JsonObject
    if (secondObj != null) {
        JschScript.fileBytesSecond =
            secondObj.entries.associate { (path, value) ->
                path to (value as JsonPrimitive).content.toByteArray(Charsets.UTF_8)
            }
    }
    val lsObj = script["ls"] as? JsonObject
    if (lsObj != null) {
        JschScript.lsEntries = lsObj.entries.associate { (dir, value) ->
            dir to (value as JsonArray).map { entry ->
                val pair = entry as JsonArray
                val name = (pair[0] as JsonPrimitive).content
                val isDir = (pair[1] as JsonPrimitive).content == "true"
                name to isDir
            }
        }
    }
}

private fun sshEventsJson(): JsonArray = buildJsonArray {
    JschScript.events.forEach { event ->
        add(
            when (event) {
                is JschEvent.SetConfig -> buildJsonObject {
                    put("event", "set_config"); put("key", event.key); put("value", event.value)
                }
                is JschEvent.SetPassword -> buildJsonObject {
                    put("event", "set_password"); put("password", event.password)
                }
                is JschEvent.HostKeyCheck -> buildJsonObject {
                    put("event", "host_key_check")
                    put("host", event.host)
                    put("decision", event.decision)
                }
                is JschEvent.Connect -> buildJsonObject {
                    put("event", "connect"); put("channel", event.channel)
                    put("timeout_ms", event.timeoutMs)
                }
                is JschEvent.OpenChannel -> buildJsonObject {
                    put("event", "open_channel"); put("kind", event.kind)
                }
                is JschEvent.SetCommand -> buildJsonObject {
                    put("event", "set_command"); put("command", event.command)
                }
                is JschEvent.Disconnect -> buildJsonObject {
                    put("event", "disconnect"); put("channel", event.channel)
                }
                is JschEvent.Stat -> buildJsonObject {
                    put("event", "stat"); put("path", event.path); put("found", event.found)
                }
                is JschEvent.Mkdir -> buildJsonObject {
                    put("event", "mkdir"); put("path", event.path)
                }
                is JschEvent.Get -> buildJsonObject {
                    put("event", "get"); put("path", event.path)
                }
                is JschEvent.Put -> buildJsonObject {
                    put("event", "put"); put("path", event.path); put("bytes", event.bytes)
                }
                is JschEvent.Ls -> buildJsonObject {
                    put("event", "ls"); put("dir", event.dir)
                }
                is JschEvent.Pwd -> buildJsonObject {
                    put("event", "pwd"); put("path", event.path)
                }
            },
        )
    }
}

internal fun sshExecute(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    val command = scenario.string("command")
    val workdir = scenario.optionalString("workdir") ?: ""
    val timeoutMs = scenario.int("timeout_ms")
    val attempt = attempt<String> { backend.executeCommand(command, workdir, timeoutMs) }
    backend.close()
    val captured = JschScript.events
        .filterIsInstance<JschEvent.HostKeyCheck>()
        .lastOrNull()
        ?.presentedB64
    return buildJsonObject {
        put("result", resultOrNull(attempt.result))
        put("threw", resultOrNull(attempt.threw))
        if (captured == null) put("captured_host_key", JsonNull)
        else put("captured_host_key", captured)
        put("ssh_events", sshEventsJson())
    }
}

internal fun sshExecuteEvents(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    val command = scenario.string("command")
    val workdir = scenario.optionalString("workdir") ?: ""
    val timeoutMs = scenario.int("timeout_ms")
    val eventList = mutableListOf<JsonElement>()
    val failure: String? = try {
        runBlocking {
            backend.executeCommandEvents(command, workdir, timeoutMs).collect { event ->
                eventList.add(
                    when (event) {
                        is ToolExecutionEvent.Completed -> buildJsonObject {
                            put("type", "completed")
                            put("text", event.result.text)
                            put("is_error", event.result.isError)
                        }
                        else -> buildJsonObject {
                            put("type", "other")
                            put("text", event.toString())
                        }
                    },
                )
            }
        }
        null
    } catch (thrown: Exception) {
        thrown.message ?: ""
    }
    backend.close()
    return buildJsonObject {
        if (failure == null) put("events", buildJsonArray { eventList.forEach { add(it) } })
        else put("events", JsonNull)
        put("threw", resultOrNull(failure))
        put("ssh_events", sshEventsJson())
    }
}

internal fun sshFingerprint(scenario: JsonObject): JsonElement {
    JschScript.reset()
    val fingerprint = SshClient.fingerprintSha256(scenario.string("base64_key"))
    return buildJsonObject { put("result", fingerprint) }
}

internal fun sshFileRead(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    val attempt = attempt<com.newoether.agora.util.ShellFileReadResult> {
        backend.fileRead(
            scenario.string("path"),
            scenario.optionalLong("offset") ?: 0,
            scenario.optionalLong("limit") ?: 0,
        )
    }
    backend.close()
    val result = attempt.result
    return buildJsonObject {
        if (result == null) put("result", JsonNull) else put("result", buildJsonObject {
            put("content", result.content)
            put("lines", result.lines)
            put("total_lines", result.totalLines)
            put("total_bytes", result.totalBytes)
            put("returned_bytes", result.returnedBytes)
            put("offset", result.offset)
            put("limit", result.limit)
            put("truncated", result.truncated)
            if (result.error == null) put("error", JsonNull) else put("error", result.error)
        })
        put("threw", resultOrNull(attempt.threw))
        put("ssh_events", sshEventsJson())
    }
}

internal fun sshFileWrite(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    val attempt = attempt<String?> {
        backend.fileWrite(scenario.string("path"), scenario.string("content"))
    }
    backend.close()
    return buildJsonObject {
        put("result", resultOrNull(attempt.result))
        put("threw", resultOrNull(attempt.threw))
        put("ssh_events", sshEventsJson())
    }
}

internal fun sshFileEdit(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    val attempt = attempt<com.newoether.agora.util.ShellFileEditResult> {
        backend.fileEdit(
            scenario.string("path"),
            scenario.string("old_string"),
            scenario.string("new_string"),
            scenario.optionalBoolean("replace_all") ?: false,
        )
    }
    backend.close()
    val result = attempt.result
    return buildJsonObject {
        if (result == null) put("result", JsonNull) else put("result", buildJsonObject {
            put("replacements", result.replacements)
            put("sha256", result.sha256)
            if (result.error == null) put("error", JsonNull) else put("error", result.error)
        })
        put("threw", resultOrNull(attempt.threw))
        put("ssh_events", sshEventsJson())
    }
}

internal fun sshFileGlob(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    var outResult: JsonElement = JsonNull
    var outThrew: String? = null
    try {
        val outcome = runBlocking {
            backend.fileGlob(
                scenario.string("pattern"),
                scenario.optionalString("path") ?: "",
                scenario.optionalLong("depth")?.toInt(),
            )
        }
        outcome.fold(
            onSuccess = { (files, truncated) ->
                outResult = buildJsonObject {
                    put("files", buildJsonArray { files.forEach { add(it) } })
                    put("truncated", truncated)
                }
            },
            onFailure = { failure -> outThrew = failure.message ?: "" },
        )
    } catch (thrown: Exception) {
        outThrew = thrown.message ?: ""
    }
    backend.close()
    return buildJsonObject {
        put("result", outResult)
        put("threw", resultOrNull(outThrew))
        put("ssh_events", sshEventsJson())
    }
}

internal fun sshFileGrep(scenario: JsonObject): JsonElement {
    installJschScript(scenario)
    val backend = SshBackend(scenario.device())
    var outResult: JsonElement = JsonNull
    var outThrew: String? = null
    try {
        val outcome = runBlocking {
            backend.fileGrep(
                scenario.string("pattern"),
                scenario.optionalString("path") ?: "",
                scenario.optionalString("glob") ?: "",
            )
        }
        outcome.fold(
            onSuccess = { (matches, truncated) ->
                outResult = buildJsonObject {
                    put("matches", buildJsonArray {
                        matches.forEach { match ->
                            add(buildJsonObject {
                                put("path", match.path)
                                put("line", match.line)
                                put("content", match.content)
                            })
                        }
                    })
                    put("truncated", truncated)
                }
            },
            onFailure = { failure -> outThrew = failure.message ?: "" },
        )
    } catch (thrown: Exception) {
        outThrew = thrown.message ?: ""
    }
    backend.close()
    return buildJsonObject {
        put("result", outResult)
        put("threw", resultOrNull(outThrew))
        put("ssh_events", sshEventsJson())
    }
}

