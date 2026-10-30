// Golden trace driver for tool/ShellToolBackends.kt.
//
// ShellToolBackends.kt compiles VERBATIM over the pinned leaf modules (ShellClient,
// ConchSseParser, ShellToolJson, SshClient, ShellCrypto, ConchHandshake, PortableGlobMatcher,
// Constants, the SandboxManager interface) and the stand doubles: the api/HttpClient streaming
// surface, ConchNetwork, a JSch subset whose host-key checks run SshClient's REAL
// HostKeyRepository implementation, and a scripted SandboxManager. The trace pins:
//  - ConchBackend: URL/fetch guards, prepareRequest-driven streamPost invocation (url, masked
//    headers, line/error limits), the execute JSON (exit_code | execution_error/warning/
//    output), the streaming deltas + Completed ordering, and every failure wrap;
//  - SshBackend: the assembled exec command (workdir cd + escapeBash), TOFU decisions and
//    capturedHostKey, stdout+stderr trimEnd formatting, the timeout, fingerprintSha256, and the
//    SFTP flows (file read incl. the complete-content second read, write with parent mkdirs,
//    edit incl. the concurrent-change guard, grep server/fallback paths, glob with pwd fallback);
//  - SandboxBackend: the not-installed guard, warning field and its failure wraps.
// The SshBackend section lives in ShellToolBackendsTraceDumpSsh.kt (same package),
// split out to satisfy the 800-line Kotlin source budget.
package shell.tool.backends.golden

import com.jcraft.jsch.JschEvent
import com.jcraft.jsch.JschScript
import com.newoether.agora.api.HttpClient
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.sandbox.ScriptedSandboxManager
import com.newoether.agora.tool.ConchBackend
import com.newoether.agora.tool.SandboxBackend
import com.newoether.agora.tool.SshBackend
import com.newoether.agora.tool.ToolExecutionEvent
import com.newoether.agora.util.ConchNetwork
import com.newoether.agora.util.SshClient
import java.io.File
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
        "conch_execute" -> conchExecuteOutcome(scenario, events = false)
        "conch_execute_events" -> conchExecuteOutcome(scenario, events = true)
        "conch_file_read" -> conchFileRead(scenario)
        "conch_file_write" -> conchFileWrite(scenario)
        "conch_file_edit" -> conchFileEdit(scenario)
        "conch_file_glob" -> conchFileGlob(scenario)
        "conch_file_grep" -> conchFileGrep(scenario)
        "ssh_execute" -> sshExecute(scenario)
        "ssh_execute_events" -> sshExecuteEvents(scenario)
        "ssh_fingerprint" -> sshFingerprint(scenario)
        "ssh_file_read" -> sshFileRead(scenario)
        "ssh_file_write" -> sshFileWrite(scenario)
        "ssh_file_edit" -> sshFileEdit(scenario)
        "ssh_file_glob" -> sshFileGlob(scenario)
        "ssh_file_grep" -> sshFileGrep(scenario)
        "sandbox_execute" -> sandboxExecute(scenario)
        "sandbox_file_read" -> sandboxFileRead(scenario)
        "sandbox_file_write" -> sandboxFileWrite(scenario)
        "sandbox_file_edit" -> sandboxFileEdit(scenario)
        "sandbox_file_glob" -> sandboxFileGlob(scenario)
        "sandbox_file_grep" -> sandboxFileGrep(scenario)
        else -> error("unknown op $op")
    }
    return buildJsonObject {
        put("op", op)
        put("scenario", scenario)
        put("outcome", outcome)
    }.toString()
}

// ---- Shared scenario helpers ---------------------------------------------------------------------

internal fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

internal fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content }

internal fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.long(name: String): Long = string(name).toLong()

internal fun JsonObject.optionalLong(name: String): Long? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toLongOrNull() }

internal fun JsonObject.optionalBoolean(name: String): Boolean? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toBooleanStrictOrNull() }

private fun JsonObject.strings(name: String): List<String> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { (it as JsonPrimitive).content }
}

internal fun JsonObject.device(): ShellDeviceConfig {
    val obj = this["device"] as? JsonObject ?: error("missing device")
    return ShellDeviceConfig(
        name = obj.string("name"),
        description = obj.optionalString("description") ?: "",
        type = obj.optionalString("type") ?: "conch",
        serverUrl = obj.optionalString("serverUrl") ?: "",
        apiKey = obj.optionalString("apiKey") ?: "",
        conchPublicKey = obj.optionalString("conchPublicKey") ?: "",
        sshHost = obj.optionalString("sshHost") ?: "",
        sshPort = obj.optionalInt() ?: 22,
        sshUser = obj.optionalString("sshUser") ?: "root",
        sshPassword = obj.optionalString("sshPassword") ?: "",
        sshHostKey = obj.optionalString("sshHostKey") ?: "",
    )
}

private fun JsonObject.optionalInt(): Int? =
    (this as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toIntOrNull() }

internal fun resultOrNull(value: String?): JsonElement =
    if (value == null) JsonNull else JsonPrimitive(value)

/** Runs a suspending backend call, mapping exceptions the way the trace records them. */
internal class Attempt<T>(val result: T?, val threw: String?)

internal fun <T> attempt(block: suspend () -> T): Attempt<T> = try {
    Attempt(runBlocking { block() }, null)
} catch (thrown: Exception) {
    Attempt(null, thrown.message ?: "")
}

// ---- ConchNetwork script (handshake + file endpoint responses) -----------------------------------

private fun handshakeJson(apiKey: String, challenge: String, signWith: String? = null): String {
    val key = signWith ?: apiKey
    val pub = "ChlxST1g8D4sd0DzZmQdk76qjaeoeyJxGaY7a-g1W3k"
    val features = "hkdf-extract-v2,response-auth-v1,sse-sequence-v1,rejection-auth-v1"
    return buildJsonObject {
        put("public_key", pub)
        put("nonce", "stand-nonce")
        put("signature", hmacHex(key, "stand-nonce|$pub"))
        put("features", features)
        put("challenge", challenge)
        put("features_signature", hmacHex(key, "stand-nonce|conch-handshake-v2|$pub|$challenge|$features"))
    }.toString()
}

private fun hmacHex(key: String, payload: String): String {
    val mac = javax.crypto.Mac.getInstance("HmacSHA256")
    mac.init(javax.crypto.spec.SecretKeySpec(key.toByteArray(Charsets.UTF_8), "HmacSHA256"))
    return mac.doFinal(payload.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }
}

/** Queues handshake (when the device has an api key) plus the scripted endpoint response. */
private fun installConchFileScript(scenario: JsonObject) {
    ConchNetwork.reset()
    val device = scenario["device"] as? JsonObject
    val apiKey = device?.let { (it as JsonObject).optionalString("apiKey") } ?: ""
    ConchNetwork.handshakeBuilder = { challenge -> handshakeJson(apiKey, challenge) }
    if (apiKey.isNotEmpty()) {
        ConchNetwork.queue.addLast(ConchNetwork.ScriptedResponse(dynamicHandshake = true))
    }
    val response = scenario["response"] as? JsonObject ?: return
    ConchNetwork.queue.addLast(
        ConchNetwork.ScriptedResponse(
            code = response.optionalLong("code")?.toInt() ?: 200,
            body = response.optionalString("body") ?: "",
            rawBody = response.optionalString("raw_body"),
            signatureMode = response.optionalString("signature") ?: "absent",
            failure = response.optionalString("failure"),
            failureDetail = response.optionalString("failure_detail") ?: "",
        ),
    )
}

private fun conchRequestsJson(): JsonArray = buildJsonArray {
    ConchNetwork.requests.forEach { request ->
        val encrypted = request.headers.any { (key, _) -> key == "X-Encryption" }
        add(buildJsonObject {
            put("url", maskUrl(request.url))
            put("method", request.method)
            put("body", if (encrypted) "<encrypted>" else request.body)
            put("headers", buildJsonArray {
                request.headers.forEach { (key, value) ->
                    add(buildJsonArray { add(key); add(maskHeader(key, value)) })
                }
            })
            if (request.callTimeoutMillis == null) put("call_timeout_ms", JsonNull)
            else put("call_timeout_ms", request.callTimeoutMillis)
        })
    }
}

private fun maskHeader(key: String, value: String): String = when (key) {
    "X-Timestamp" -> "<timestamp>"
    "X-Signature" -> "<signature>"
    "X-Nonce" -> "<nonce>"
    "X-Client-Public-Key" -> "<client-public-key>"
    else -> value
}

private fun maskUrl(url: String): String =
    if (url.contains("challenge=")) url.replace(Regex("challenge=[^&]*"), "challenge=<challenge>")
    else url

private fun streamCallsJson(): JsonArray = buildJsonArray {
    HttpClient.calls.forEach { call ->
        add(buildJsonObject {
            put("url", call.url)
            put("headers", buildJsonArray {
                call.headers.forEach { (key, value) ->
                    add(buildJsonArray { add(key); add(maskHeader(key, value)) })
                }
            })
            if (call.maxLineBytes == null) put("max_line_bytes", JsonNull)
            else put("max_line_bytes", call.maxLineBytes)
            if (call.maxErrorBytes == null) put("max_error_bytes", JsonNull)
            else put("max_error_bytes", call.maxErrorBytes)
        })
    }
}

// ---- ConchBackend ---------------------------------------------------------------------------------

private fun installStreamScript(scenario: JsonObject) {
    HttpClient.reset()
    val stream = scenario["stream"] as? JsonObject ?: return
    if (stream.optionalString("failure") != null) {
        HttpClient.queue.addLast(
            HttpClient.ScriptedResponse(
                failure = stream.string("failure"),
                failureDetail = stream.optionalString("failure_detail") ?: "",
            ),
        )
        return
    }
    HttpClient.queue.addLast(
        HttpClient.ScriptedResponse(
            code = stream.optionalLong("code")?.toInt() ?: 200,
            errorBody = stream.optionalString("error_body"),
            lines = stream.strings("lines"),
        ),
    )
}

private fun conchExecuteOutcome(scenario: JsonObject, events: Boolean): JsonElement {
    ConchNetwork.reset()
    HttpClient.reset()
    val device = scenario.device()
    val apiKey = device.apiKey
    val signWith = scenario.optionalString("handshake_sign_with")
    ConchNetwork.handshakeBuilder = { challenge -> handshakeJson(apiKey, challenge, signWith) }
    if (apiKey.isNotEmpty() && scenario.optionalBoolean("script_handshake") != false) {
        ConchNetwork.queue.addLast(ConchNetwork.ScriptedResponse(dynamicHandshake = true))
    }
    installStreamScript(scenario)
    val backend = ConchBackend(device)
    val command = scenario.string("command")
    val workdir = scenario.optionalString("workdir") ?: ""
    val timeoutMs = scenario.int("timeout_ms")

    if (events) {
        val eventList = mutableListOf<JsonElement>()
        val failure: String? = try {
            val flow = backend.executeCommandEvents(command, workdir, timeoutMs)
            runBlocking {
                flow.collect { event ->
                    eventList.add(
                        when (event) {
                            is ToolExecutionEvent.OutputDelta ->
                                buildJsonObject { put("type", "delta"); put("text", event.text) }
                            is ToolExecutionEvent.Completed ->
                                buildJsonObject {
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
        return buildJsonObject {
            if (failure == null) put("events", buildJsonArray { eventList.forEach { add(it) } })
            else put("events", JsonNull)
            if (failure == null) put("threw", JsonNull) else put("threw", failure)
            put("stream_calls", streamCallsJson())
            put("requests", conchRequestsJson())
            put("closed", HttpClient.closed)
        }
    }

    val attempt = attempt<String> { backend.executeCommand(command, workdir, timeoutMs) }
    return buildJsonObject {
        put("result", resultOrNull(attempt.result))
        put("threw", resultOrNull(attempt.threw))
        put("stream_calls", streamCallsJson())
        put("requests", conchRequestsJson())
        put("closed", HttpClient.closed)
    }
}

private fun conchFileRead(scenario: JsonObject): JsonElement {
    val device = scenario.device()
    installConchFileScript(scenario)
    val backend = ConchBackend(device)
    val attempt = attempt<com.newoether.agora.util.ShellFileReadResult> {
        backend.fileRead(
            scenario.string("path"),
            scenario.optionalLong("offset") ?: 0,
            scenario.optionalLong("limit") ?: 0,
        )
    }
    if (attempt.threw != null) {
        return buildJsonObject {
            put("result", JsonNull)
            put("threw", attempt.threw)
            put("requests", conchRequestsJson())
        }
    }
    return try {
        val result = attempt.result!!
        buildJsonObject {
            put("result", buildJsonObject {
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
            put("threw", JsonNull)
            put("requests", conchRequestsJson())
        }
    } catch (thrown: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", thrown.message ?: "")
            put("requests", conchRequestsJson())
        }
    }
}

private fun conchFileWrite(scenario: JsonObject): JsonElement {
    val device = scenario.device()
    installConchFileScript(scenario)
    val backend = ConchBackend(device)
    val writeAttempt = attempt<String?> {
        backend.fileWrite(scenario.string("path"), scenario.string("content"))
    }
    if (writeAttempt.threw != null) {
        return buildJsonObject {
            put("result", JsonNull)
            put("threw", writeAttempt.threw)
            put("requests", conchRequestsJson())
        }
    }
    return try {
        val error = writeAttempt.result
        buildJsonObject {
            put("result", resultOrNull(error))
            put("threw", JsonNull)
            put("requests", conchRequestsJson())
        }
    } catch (thrown: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", thrown.message ?: "")
            put("requests", conchRequestsJson())
        }
    }
}

private fun conchFileEdit(scenario: JsonObject): JsonElement {
    val device = scenario.device()
    installConchFileScript(scenario)
    val backend = ConchBackend(device)
    val editAttempt = attempt<com.newoether.agora.util.ShellFileEditResult> {
        backend.fileEdit(
            scenario.string("path"),
            scenario.string("old_string"),
            scenario.string("new_string"),
            scenario.optionalBoolean("replace_all") ?: false,
        )
    }
    if (editAttempt.threw != null) {
        return buildJsonObject {
            put("result", JsonNull)
            put("threw", editAttempt.threw)
            put("requests", conchRequestsJson())
        }
    }
    return try {
        val result = editAttempt.result!!
        buildJsonObject {
            put("result", buildJsonObject {
                put("replacements", result.replacements)
                put("sha256", result.sha256)
                if (result.error == null) put("error", JsonNull) else put("error", result.error)
            })
            put("threw", JsonNull)
            put("requests", conchRequestsJson())
        }
    } catch (thrown: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", thrown.message ?: "")
            put("requests", conchRequestsJson())
        }
    }
}

private fun conchFileGlob(scenario: JsonObject): JsonElement {
    val device = scenario.device()
    installConchFileScript(scenario)
    val backend = ConchBackend(device)
    return try {
        val result = runBlocking {
            backend.fileGlob(
                scenario.string("pattern"),
                scenario.optionalString("path") ?: "",
                scenario.optionalLong("depth")?.toInt(),
            )
        }
        val folded = result.fold(
            onSuccess = { (files, truncated) ->
                buildJsonObject {
                    put("result", buildJsonObject {
                        put("files", buildJsonArray { files.forEach { add(it) } })
                        put("truncated", truncated)
                    })
                    put("threw", JsonNull)
                }
            },
            onFailure = { failure ->
                buildJsonObject {
                    put("result", JsonNull)
                    put("threw", failure.message ?: "")
                }
            },
        )
        buildJsonObject {
            folded.jsonObject.forEach { (key, value) -> put(key, value) }
            put("requests", conchRequestsJson())
        }
    } catch (thrown: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", thrown.message ?: "")
            put("requests", conchRequestsJson())
        }
    }
}

private fun conchFileGrep(scenario: JsonObject): JsonElement {
    val device = scenario.device()
    installConchFileScript(scenario)
    val backend = ConchBackend(device)
    return try {
        val result = runBlocking {
            backend.fileGrep(
                scenario.string("pattern"),
                scenario.optionalString("path") ?: "",
                scenario.optionalString("glob") ?: "",
            )
        }
        val folded = result.fold(
            onSuccess = { (matches, truncated) ->
                buildJsonObject {
                    put("result", buildJsonObject {
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
                    })
                    put("threw", JsonNull)
                }
            },
            onFailure = { failure ->
                buildJsonObject {
                    put("result", JsonNull)
                    put("threw", failure.message ?: "")
                }
            },
        )
        buildJsonObject {
            folded.jsonObject.forEach { (key, value) -> put(key, value) }
            put("requests", conchRequestsJson())
        }
    } catch (thrown: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", thrown.message ?: "")
            put("requests", conchRequestsJson())
        }
    }
}

// ---- SandboxBackend ---------------------------------------------------------------------------------

private val sandboxManager = ScriptedSandboxManager()

private fun installSandboxScript(scenario: JsonObject) {
    sandboxManager.clearScript()
    val script = scenario["sandbox"] as? JsonObject ?: return
    sandboxManager.available = script.optionalBoolean("available") ?: true
    script.optionalString("execute_failure")?.let { sandboxManager.executeFailure = it }
    (script["execute_result"] as? JsonObject)?.let { obj ->
        sandboxManager.executeResult = com.newoether.agora.sandbox.SandboxManager.SandboxResult(
            stdout = obj.optionalString("stdout") ?: "",
            stderr = obj.optionalString("stderr") ?: "",
            exitCode = obj.optionalLong("exit_code")?.toInt() ?: 0,
            warning = obj.optionalString("warning"),
        )
    }
    script.optionalString("file_read_failure")?.let { sandboxManager.fileReadFailure = it }
    (script["file_read_result"] as? JsonObject)?.let { obj ->
        sandboxManager.fileReadResult = com.newoether.agora.util.ShellFileReadResult(
            content = obj.optionalString("content") ?: "",
            lines = obj.optionalLong("lines")?.toInt() ?: 0,
            totalLines = obj.optionalLong("total_lines")?.toInt() ?: 0,
            totalBytes = obj.optionalLong("total_bytes") ?: 0,
            returnedBytes = obj.optionalLong("returned_bytes") ?: 0,
            offset = obj.optionalLong("offset") ?: 0,
            limit = obj.optionalLong("limit") ?: 0,
            truncated = obj.optionalBoolean("truncated") ?: false,
            error = obj.optionalString("error"),
        )
    }
    script.optionalString("file_write_failure")?.let { sandboxManager.fileWriteFailure = it }
    script.optionalString("file_write_error")?.let { sandboxManager.fileWriteError = it }
    script.optionalString("file_edit_failure")?.let { sandboxManager.fileEditFailure = it }
    (script["file_edit_result"] as? JsonObject)?.let { obj ->
        sandboxManager.fileEditResult = com.newoether.agora.util.ShellFileEditResult(
            replacements = obj.optionalLong("replacements")?.toInt() ?: 0,
            sha256 = obj.optionalString("sha256") ?: "",
            error = obj.optionalString("error"),
        )
    }
    script.optionalString("file_glob_failure")?.let { sandboxManager.fileGlobFailure = it }
    (script["file_glob_result"] as? JsonObject)?.let { obj ->
        sandboxManager.fileGlobResult =
            obj.strings("files") to (obj.optionalBoolean("truncated") ?: false)
    }
    script.optionalString("file_grep_failure")?.let { sandboxManager.fileGrepFailure = it }
    (script["file_grep_result"] as? JsonObject)?.let { obj ->
        val matches = (obj["matches"] as? JsonArray)?.map { element ->
            val entry = element as JsonObject
            com.newoether.agora.sandbox.SandboxManager.GrepMatch(
                path = entry.optionalString("path") ?: "",
                line = entry.optionalLong("line")?.toInt() ?: 0,
                content = entry.optionalString("content") ?: "",
            )
        } ?: emptyList()
        sandboxManager.fileGrepResult = matches to (obj.optionalBoolean("truncated") ?: false)
    }
}

private fun sandboxExecute(scenario: JsonObject): JsonElement {
    installSandboxScript(scenario)
    val backend = SandboxBackend(sandboxManager)
    val command = scenario.string("command")
    val workdir = scenario.optionalString("workdir") ?: ""
    val timeoutMs = scenario.int("timeout_ms")
    val attempt = attempt<String> { backend.executeCommand(command, workdir, timeoutMs) }
    return buildJsonObject {
        put("result", resultOrNull(attempt.result))
        put("threw", resultOrNull(attempt.threw))
        put("sandbox_calls", buildJsonArray { sandboxManager.calls.forEach { add(it) } })
    }
}

private fun sandboxFileRead(scenario: JsonObject): JsonElement {
    installSandboxScript(scenario)
    val backend = SandboxBackend(sandboxManager)
    val result = runBlocking {
        backend.fileRead(
            scenario.string("path"),
            scenario.optionalLong("offset") ?: 0,
            scenario.optionalLong("limit") ?: 0,
        )
    }
    return buildJsonObject {
        put("result", buildJsonObject {
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
        put("sandbox_calls", buildJsonArray { sandboxManager.calls.forEach { add(it) } })
    }
}

private fun sandboxFileWrite(scenario: JsonObject): JsonElement {
    installSandboxScript(scenario)
    val backend = SandboxBackend(sandboxManager)
    val error = runBlocking { backend.fileWrite(scenario.string("path"), scenario.string("content")) }
    return buildJsonObject {
        put("result", resultOrNull(error))
        put("sandbox_calls", buildJsonArray { sandboxManager.calls.forEach { add(it) } })
    }
}

private fun sandboxFileEdit(scenario: JsonObject): JsonElement {
    installSandboxScript(scenario)
    val backend = SandboxBackend(sandboxManager)
    val result = runBlocking {
        backend.fileEdit(
            scenario.string("path"),
            scenario.string("old_string"),
            scenario.string("new_string"),
            scenario.optionalBoolean("replace_all") ?: false,
        )
    }
    return buildJsonObject {
        put("result", buildJsonObject {
            put("replacements", result.replacements)
            put("sha256", result.sha256)
            if (result.error == null) put("error", JsonNull) else put("error", result.error)
        })
        put("sandbox_calls", buildJsonArray { sandboxManager.calls.forEach { add(it) } })
    }
}

private fun sandboxFileGlob(scenario: JsonObject): JsonElement {
    installSandboxScript(scenario)
    val backend = SandboxBackend(sandboxManager)
    val result = runBlocking {
        backend.fileGlob(
            scenario.string("pattern"),
            scenario.optionalString("path") ?: "",
            scenario.optionalLong("depth")?.toInt(),
        )
    }
    val folded = result.fold(
        onSuccess = { (files, truncated) ->
            buildJsonObject {
                put("result", buildJsonObject {
                    put("files", buildJsonArray { files.forEach { add(it) } })
                    put("truncated", truncated)
                })
                put("threw", JsonNull)
            }
        },
        onFailure = { failure ->
            buildJsonObject {
                put("result", JsonNull)
                put("threw", failure.message ?: "")
            }
        },
    )
    return buildJsonObject {
        folded.jsonObject.forEach { (key, value) -> put(key, value) }
        put("sandbox_calls", buildJsonArray { sandboxManager.calls.forEach { add(it) } })
    }
}

private fun sandboxFileGrep(scenario: JsonObject): JsonElement {
    installSandboxScript(scenario)
    val backend = SandboxBackend(sandboxManager)
    val result = runBlocking {
        backend.fileGrep(
            scenario.string("pattern"),
            scenario.optionalString("path") ?: "",
            scenario.optionalString("glob") ?: "",
        )
    }
    val folded = result.fold(
        onSuccess = { (matches, truncated) ->
            buildJsonObject {
                put("result", buildJsonObject {
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
                })
                put("threw", JsonNull)
            }
        },
        onFailure = { failure ->
            buildJsonObject {
                put("result", JsonNull)
                put("threw", failure.message ?: "")
            }
        },
    )
    return buildJsonObject {
        folded.jsonObject.forEach { (key, value) -> put(key, value) }
        put("sandbox_calls", buildJsonArray { sandboxManager.calls.forEach { add(it) } })
    }
}
