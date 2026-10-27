// Golden trace driver for tool/ShellToolProvider.kt.
//
// ShellToolProvider.kt compiles VERBATIM from the app tree over the pinned leaf modules
// (ShellClient/ConchSseParser/ShellToolJson/SshClient/ShellCrypto/ConchHandshake/
// PortableGlobMatcher/Constants/SandboxManager/SandboxManagerFactory + the ChatMessage model
// chain + the ToolProvider contracts) and the already-pinned backends (ShellToolBackends.kt),
// definitions builder (ShellToolDefinitions.kt) and durable-job executor
// (ShellDurableJobExecutor.kt). Platform substitutes (see README):
//   1. platform/HttpStream.kt            — the api/HttpClient.kt streaming surface with a
//      recorded invocation log and SSE-line sealing on X-Encryption;
//   2. platform/ConchNetwork.kt          — the GET/POST transport plus streamCalls/limits;
//   3. platform/JSch.kt                  — the JSch subset SshClient.kt drives;
//   4. platform/ScriptedSandboxManager.kt — scripted SandboxManager;
//   5. platform/ScriptedSandboxManagerFactory.kt — scripted factory;
//   6. platform/ToolImageStore.kt        — scripted image persistence;
//   7. platform/{GenerationContext,ShellDeviceConfig,ToolImageStore,DebugLog,Base64,
//      Immutable,MarkdownState}.
//
// The trace pins the provider's own layer:
//  - definitions() delegation to ShellToolDefinitions for every device/sandbox combination;
//  - list_shells (Local Sandbox availability, blank-name fallback, ssh/conch fields);
//  - execute_shell_command validation (command/timeout/workdir budgets, clamp, server
//    resolution, ambiguous and unknown servers) and confirmation gating (remote device,
//    shared-storage sandbox, denied_by_user wording);
//  - Conch foreground dispatch (durable job, terminal first poll), background start/stop,
//    job listing/getting/waiting, and the encrypted execute path;
//  - SSH and Sandbox dispatch through the real backends, including their error wraps;
//  - file_read/write/edit/glob/grep on all three targets;
//  - view_image persistence and its error branches;
//  - acknowledgeCommittedJobs (dedup, running-job skip, failure swallowing, unknown server).
//
// Real-clock values embedded by the durable-job loops ("waited_ms") are masked: the frozen
// trace must not depend on wall-clock scheduling.
package shell.tool.provider.golden

import com.jcraft.jsch.JschEvent
import com.jcraft.jsch.JschScript
import com.newoether.agora.api.HttpClient
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.model.ToolCallData
import com.newoether.agora.model.ToolImageAttachment
import com.newoether.agora.sandbox.SandboxManager
import com.newoether.agora.sandbox.ScriptedSandboxManager
import com.newoether.agora.sandbox.ScriptedSandboxManagerFactory
import com.newoether.agora.tool.ShellToolProvider
import com.newoether.agora.tool.ToolExecutionEvent
import com.newoether.agora.tool.ToolExecutionResult
import com.newoether.agora.tool.ToolImageStore
import com.newoether.agora.util.ConchNetwork
import com.newoether.agora.util.ShellFileEditResult
import com.newoether.agora.util.ShellFileReadResult
import com.newoether.agora.viewmodel.GenerationContext
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
                .writeText(maskTimings(render(scenario)) + "\n")
        }
        return
    }
    println(maskTimings(render(File(args[0]))))
}

/** Real-clock durable-job values must not leak into the frozen trace. */
private val WAITED_MS = Regex("(waited_ms\\\\?\":)\\d+")

private fun maskTimings(text: String): String =
    WAITED_MS.replace(text) { match -> match.groupValues[1] + "MASKED" }

// ---- JSON helpers ---------------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content }

private fun JsonObject.optionalLong(name: String): Long? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toLongOrNull() }

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    (this[name] as? JsonPrimitive)?.let {
        if (it == JsonNull) null else it.content.toBooleanStrictOrNull()
    }

private fun JsonObject.strings(name: String): List<String> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { (it as JsonPrimitive).content }
}

private fun JsonObject.optionalObject(name: String): JsonObject? = this[name] as? JsonObject

private fun resultOrNull(value: String?): JsonElement =
    if (value == null) JsonNull else JsonPrimitive(value)

private class Attempt<T>(val result: T?, val threw: String?)

private fun <T> attempt(block: suspend () -> T): Attempt<T> = try {
    Attempt(runBlocking { block() }, null)
} catch (thrown: Exception) {
    Attempt(null, thrown.message ?: thrown.javaClass.simpleName)
}

// ---- Scenario construction ------------------------------------------------------------------------

private fun render(scenarioFile: File): String {
    val scenario = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = scenario.string("op")
    val ctx = scenario.context()

    val sandboxFactory = ScriptedSandboxManagerFactory()
    installSandboxScript(scenario.optionalObject("sandbox"), sandboxFactory)
    val imageStore = if (scenario.optionalBoolean("no_image_store") == true) null else ToolImageStore()
    installImageScript(scenario.optionalObject("image"), imageStore)
    installConchScript(scenario)
    installStreamScript(scenario.optionalObject("stream"))
    installJschScript(scenario.optionalObject("jsch"))

    val provider = ShellToolProvider(sandboxFactory, imageStore)
    val confirmCalls = mutableListOf<JsonElement>()
    val allowed = scenario.optionalBoolean("confirm_allowed") ?: true
    provider.confirm = { server, summary, conversationId ->
        confirmCalls.add(buildJsonObject {
            put("server", server)
            put("summary", summary)
            put("conversation_id", conversationId?.let(::JsonPrimitive) ?: JsonNull)
        })
        allowed
    }

    val outcome = when (op) {
        "execute" -> executeOutcome(provider, scenario, ctx)
        "events" -> eventsOutcome(provider, scenario, ctx)
        "acknowledge" -> acknowledgeOutcome(provider, scenario, ctx)
        "definitions" -> definitionsJson(provider, ctx)
        else -> error("unknown op $op")
    }

    return buildJsonObject {
        put("op", op)
        put("outcome", outcome)
        if (op != "definitions") put("definitions", definitionsJson(provider, ctx))
        put("confirm_calls", buildJsonArray { confirmCalls.forEach { add(it) } })
        put("conch_requests", conchRequestsJson())
        put("http_calls", streamCallsJson())
        put("http_closed", HttpClient.closed)
        put("sandbox_calls", buildJsonArray {
            sandboxFactory.manager.calls.forEach { add(JsonPrimitive(it)) }
        })
        put("image_calls", buildJsonArray {
            imageStore?.calls?.forEach { add(JsonPrimitive(it)) }
        })
        put("ssh_events", sshEventsJson())
    }.toString()
}

private fun JsonObject.context(): GenerationContext {
    val obj = optionalObject("context") ?: JsonObject(emptyMap())
    val devices = (obj["shellDevices"] as? JsonArray)?.map { it.jsonObject.device() } ?: emptyList()
    return GenerationContext(
        conversationId = obj.optionalString("conversationId"),
        shellEnabled = obj.optionalBoolean("shellEnabled") ?: false,
        shellDevices = devices,
        sandboxEnabled = obj.optionalBoolean("sandboxEnabled") ?: false,
        sandboxSharedStorageEnabled = obj.optionalBoolean("sandboxSharedStorageEnabled") ?: false,
        toolTimeoutMs = obj.optionalLong("toolTimeoutMs") ?: 60_000L,
    )
}

private fun JsonObject.device(): ShellDeviceConfig = ShellDeviceConfig(
    name = optionalString("name") ?: "",
    description = optionalString("description") ?: "",
    type = optionalString("type") ?: "conch",
    serverUrl = optionalString("serverUrl") ?: "",
    apiKey = optionalString("apiKey") ?: "",
    conchPublicKey = optionalString("conchPublicKey") ?: "",
    sshHost = optionalString("sshHost") ?: "",
    sshPort = optionalLong("sshPort")?.toInt() ?: 22,
    sshUser = optionalString("sshUser") ?: "root",
    sshPassword = optionalString("sshPassword") ?: "",
    sshHostKey = optionalString("sshHostKey") ?: "",
)

// ---- Provider outcomes ----------------------------------------------------------------------------

private fun toolCall(scenario: JsonObject): Pair<String, String> {
    val call = scenario.optionalObject("tool_call") ?: error("missing tool_call")
    return call.string("name") to (call.optionalString("arguments") ?: "{}")
}

private fun executeOutcome(
    provider: ShellToolProvider,
    scenario: JsonObject,
    ctx: GenerationContext,
): JsonElement {
    val (name, arguments) = toolCall(scenario)
    val result = attempt<String> { provider.execute(name, arguments, ctx) }
    return buildJsonObject {
        put("result", resultOrNull(result.result))
        put("threw", resultOrNull(result.threw))
    }
}

private fun eventsOutcome(
    provider: ShellToolProvider,
    scenario: JsonObject,
    ctx: GenerationContext,
): JsonElement {
    val (name, arguments) = toolCall(scenario)
    val eventList = mutableListOf<JsonElement>()
    var failure: String? = null
    try {
        runBlocking {
            provider.executeEvents(name, arguments, ctx).collect { event ->
                eventList.add(eventJson(event))
            }
        }
    } catch (thrown: Exception) {
        failure = thrown.message ?: thrown.javaClass.simpleName
    }
    return buildJsonObject {
        if (failure == null) put("events", buildJsonArray { eventList.forEach { add(it) } })
        else put("events", JsonNull)
        put("threw", resultOrNull(failure))
    }
}

private fun eventJson(event: ToolExecutionEvent): JsonElement = when (event) {
    is ToolExecutionEvent.OutputDelta -> buildJsonObject {
        put("type", "delta"); put("text", event.text)
    }
    is ToolExecutionEvent.OutputSnapshot -> buildJsonObject {
        put("type", "snapshot"); put("text", event.text)
    }
    is ToolExecutionEvent.TargetResolved -> buildJsonObject {
        put("type", "target_resolved"); put("target", event.target)
    }
    is ToolExecutionEvent.Progress -> buildJsonObject {
        put("type", "progress"); put("message", event.message)
    }
    is ToolExecutionEvent.Completed -> buildJsonObject {
        put("type", "completed")
        put("text", event.result.text)
        put("is_error", event.result.isError)
        put("images", imagesJson(event.result))
    }
}

private fun imagesJson(result: ToolExecutionResult): JsonArray = buildJsonArray {
    result.images.forEach { image -> add(imageJson(image)) }
}

private fun imageJson(image: ToolImageAttachment): JsonElement = buildJsonObject {
    put("path", image.path)
    put("mime_type", image.mimeType)
    put("size", image.sizeBytes)
    put("width", image.width?.let(::JsonPrimitive) ?: JsonNull)
    put("height", image.height?.let(::JsonPrimitive) ?: JsonNull)
    put("sha256", image.sha256)
}

private fun definitionsJson(provider: ShellToolProvider, ctx: GenerationContext): JsonArray =
    buildJsonArray {
        provider.definitions(ctx).forEach { definition ->
            add(buildJsonObject {
                put("name", definition.function.name)
                put("properties", buildJsonArray {
                    definition.function.parameters.properties.keys.forEach { add(JsonPrimitive(it)) }
                })
                put("required", buildJsonArray {
                    definition.function.parameters.required.forEach { add(JsonPrimitive(it)) }
                })
            })
        }
    }

private fun acknowledgeOutcome(
    provider: ShellToolProvider,
    scenario: JsonObject,
    ctx: GenerationContext,
): JsonElement {
    val calls = (scenario["calls"] as? JsonArray)?.map { it.jsonObject } ?: emptyList()
    val records = calls.map { call ->
        ToolCallData(
            toolName = call.string("toolName"),
            arguments = call.string("arguments"),
            result = call.string("result"),
        )
    }
    val result = attempt<Unit> { provider.acknowledgeCommittedJobs(records, ctx) }
    return buildJsonObject {
        put("result", if (result.threw == null) JsonPrimitive("acknowledged") else JsonNull)
        put("threw", resultOrNull(result.threw))
    }
}

// ---- Transport scripts ----------------------------------------------------------------------------

private fun installConchScript(scenario: JsonObject) {
    ConchNetwork.reset()
    val apiKey = scenario.optionalString("conch_api_key") ?: ""
    ConchNetwork.handshakeBuilder = { challenge -> handshakeJson(apiKey, challenge) }
    if (apiKey.isNotEmpty()) {
        ConchNetwork.queue.addLast(ConchNetwork.ScriptedResponse(dynamicHandshake = true))
    }
    (scenario["conch_responses"] as? JsonArray)?.forEach { element ->
        val response = element.jsonObject
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
}

private fun installStreamScript(stream: JsonObject?) {
    HttpClient.reset()
    if (stream == null) return
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

private fun installSandboxScript(
    script: JsonObject?,
    factory: ScriptedSandboxManagerFactory,
) {
    if (script == null) return
    val manager = factory.manager
    factory.factoryAvailable = script.optionalBoolean("factory_available") ?: true
    manager.available = script.optionalBoolean("available") ?: true
    script.optionalString("execute_failure")?.let { manager.executeFailure = it }
    script.optionalObject("execute_result")?.let { result ->
        manager.executeResult = SandboxManager.SandboxResult(
            result.optionalString("stdout") ?: "",
            result.optionalString("stderr") ?: "",
            result.optionalLong("exit_code")?.toInt() ?: 0,
            result.optionalString("warning"),
        )
    }
    script.optionalString("file_read_failure")?.let { manager.fileReadFailure = it }
    script.optionalObject("file_read_result")?.let { result ->
        manager.fileReadResult = ShellFileReadResult(
            content = result.optionalString("content") ?: "",
            lines = result.optionalLong("lines")?.toInt() ?: 0,
            totalLines = result.optionalLong("total_lines")?.toInt() ?: 0,
            totalBytes = result.optionalLong("total_bytes") ?: 0,
            returnedBytes = result.optionalLong("returned_bytes") ?: 0,
            offset = result.optionalLong("offset") ?: 0,
            limit = result.optionalLong("limit") ?: 0,
            truncated = result.optionalBoolean("truncated") ?: false,
            error = result.optionalString("error"),
        )
    }
    script.optionalString("file_write_failure")?.let { manager.fileWriteFailure = it }
    script.optionalString("file_write_error")?.let { manager.fileWriteError = it }
    script.optionalString("file_edit_failure")?.let { manager.fileEditFailure = it }
    script.optionalObject("file_edit_result")?.let { result ->
        manager.fileEditResult = ShellFileEditResult(
            replacements = result.optionalLong("replacements")?.toInt() ?: 0,
            sha256 = result.optionalString("sha256") ?: "",
            error = result.optionalString("error"),
        )
    }
    script.optionalString("file_glob_failure")?.let { manager.fileGlobFailure = it }
    script.optionalObject("file_glob_result")?.let { result ->
        manager.fileGlobResult = Pair(
            result.strings("files"),
            result.optionalBoolean("truncated") ?: false,
        )
    }
    script.optionalString("file_grep_failure")?.let { manager.fileGrepFailure = it }
    script.optionalObject("file_grep_result")?.let { result ->
        val matches = (result["matches"] as? JsonArray)?.map { element ->
            val match = element.jsonObject
            SandboxManager.GrepMatch(
                match.optionalString("path") ?: "",
                match.optionalLong("line")?.toInt() ?: 0,
                match.optionalString("content") ?: "",
            )
        } ?: emptyList()
        manager.fileGrepResult = Pair(matches, result.optionalBoolean("truncated") ?: false)
    }
}

private fun installImageScript(script: JsonObject?, store: ToolImageStore?) {
    if (script == null || store == null) return
    script.optionalString("failure")?.let { store.failure = it }
    script.optionalObject("result")?.let { result ->
        store.result = ToolImageAttachment(
            path = result.optionalString("path") ?: "",
            mimeType = result.optionalString("mime_type") ?: "",
            sizeBytes = result.optionalLong("size") ?: 0,
            width = result.optionalLong("width")?.toInt(),
            height = result.optionalLong("height")?.toInt(),
            sha256 = result.optionalString("sha256") ?: "",
        )
    }
}

private fun installJschScript(script: JsonObject?) {
    JschScript.reset()
    if (script == null) return
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

    script.optionalObject("stats")?.let { stats ->
        JschScript.stats = stats.entries.associate { (path, value) ->
            val obj = value.jsonObject
            path to com.jcraft.jsch.StatScript(
                isDir = obj.optionalBoolean("is_dir") ?: false,
                size = obj.optionalLong("size") ?: 0,
            )
        }
    }
    (script["mkdir_failures"] as? JsonArray)?.let { array ->
        JschScript.mkdirFailures = array.map { (it as JsonPrimitive).content }.toSet()
    }
    script.optionalObject("put_failures")?.let { failures ->
        JschScript.putFailures = failures.entries.associate { (path, value) ->
            path to (value as JsonPrimitive).content
        }
    }
    script.optionalObject("file_bytes")?.let { files ->
        JschScript.fileBytes = files.entries.associate { (path, value) ->
            path to (value as JsonPrimitive).content.toByteArray(Charsets.UTF_8)
        }
    }
    script.optionalObject("file_bytes_second")?.let { files ->
        JschScript.fileBytesSecond = files.entries.associate { (path, value) ->
            path to (value as JsonPrimitive).content.toByteArray(Charsets.UTF_8)
        }
    }
    script.optionalObject("ls")?.let { listing ->
        JschScript.lsEntries = listing.entries.associate { (dir, value) ->
            dir to (value as JsonArray).map { entry ->
                val pair = entry as JsonArray
                (pair[0] as JsonPrimitive).content to
                    ((pair[1] as JsonPrimitive).content == "true")
            }
        }
    }
}

// ---- Recorded transports ---------------------------------------------------------------------------

private fun handshakeJson(apiKey: String, challenge: String): String {
    val pub = "ChlxST1g8D4sd0DzZmQdk76qjaeoeyJxGaY7a-g1W3k"
    val features = "hkdf-extract-v2,response-auth-v1,sse-sequence-v1,rejection-auth-v1"
    return buildJsonObject {
        put("public_key", pub)
        put("nonce", "stand-nonce")
        put("signature", hmacHex(apiKey, "stand-nonce|$pub"))
        put("features", features)
        put("challenge", challenge)
        put(
            "features_signature",
            hmacHex(apiKey, "stand-nonce|conch-handshake-v2|$pub|$challenge|$features"),
        )
    }.toString()
}

private fun hmacHex(key: String, payload: String): String {
    val mac = javax.crypto.Mac.getInstance("HmacSHA256")
    mac.init(javax.crypto.spec.SecretKeySpec(key.toByteArray(Charsets.UTF_8), "HmacSHA256"))
    return mac.doFinal(payload.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }
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
