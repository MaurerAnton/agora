// Golden trace driver for ShellClient.kt.
//
// ShellClient.kt compiles VERBATIM from the app tree together with the real ShellCrypto.kt and
// ConchHandshake.kt. The stand supplies only transport doubles: platform/ConchNetwork.kt (the
// OkHttp/okio GET/POST surface with a request log; encrypted responses are sealed with the
// exact session key ShellClient derived, because X25519 lets the double recompute the shared
// secret from the fixed stand server private key) and platform/DebugLog.kt (silent logger).
// The trace pins the handshake flow, the encrypted-request assembly (masked for its random
// nonce/timestamp/ephemeral-key parts), describeConch*Failure messages, every file endpoint's
// response coercion, the jobs payload builders and describeConchRequestFailure wrapping.
package shell.client.golden

import com.newoether.agora.util.ConchNetwork
import com.newoether.agora.util.ShellClient
import com.newoether.agora.util.ShellCrypto
import java.io.File
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
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

/** Fixed stand server identity: the handshake's public_key and the response-sealing private key. */
private const val SERVER_PUBLIC_KEY = "ChlxST1g8D4sd0DzZmQdk76qjaeoeyJxGaY7a-g1W3k"
private const val SERVER_PRIVATE_KEY_PKCS8 =
    "MC4CAQAwBQYDK2VuBCIEIHFn0CpncaiXmIxD1seMg1VuVjEsnCQC7We5bp39n6wL"
private const val REQUIRED_FEATURES =
    "hkdf-extract-v2,response-auth-v1,sse-sequence-v1,rejection-auth-v1"

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
        "fetch_public_key" -> fetchOutcome(scenario)
        "prepare_request" -> prepareOutcome(scenario)
        "decrypt_sse" -> decryptOutcome(scenario)
        "file_read" -> fileReadOutcome(scenario)
        "file_write" -> fileWriteOutcome(scenario)
        "file_edit" -> fileEditOutcome(scenario)
        "file_glob" -> fileGlobOutcome(scenario)
        "file_grep" -> fileGrepOutcome(scenario)
        "file_image" -> fileImageOutcome(scenario)
        "job_start", "job_list", "job_get", "job_stop", "job_ack" -> jobOutcome(scenario, op)
        else -> error("unknown op $op")
    }
    return buildJsonObject {
        put("op", op)
        put("scenario", scenario)
        put("outcome", outcome)
    }.toString()
}

// ---- Handshake construction (HMAC over the per-run challenge, signed with the scenario key) ------

private fun hmacHex(key: String, payload: String): String {
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec(key.toByteArray(Charsets.UTF_8), "HmacSHA256"))
    return mac.doFinal(payload.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }
}

private fun handshakeJson(
    signWith: String,
    challenge: String,
    features: String,
    challengeField: String,
    publicKey: String,
): String = buildJsonObject {
    put("public_key", publicKey)
    put("nonce", "stand-nonce")
    put("signature", hmacHex(signWith, "stand-nonce|$publicKey"))
    put("features", features)
    put("challenge", challengeField)
    put(
        "features_signature",
        hmacHex(
            signWith,
            "stand-nonce|conch-handshake-v2|$publicKey|$challengeField|$features",
        ),
    )
}.toString()

private fun scenarioHandshake(scenario: JsonObject, challenge: String): String = handshakeJson(
    signWith = scenario.optionalString("handshake_sign_with") ?: scenario.string("api_key"),
    challenge = challenge,
    features = scenario.optionalString("handshake_features") ?: REQUIRED_FEATURES,
    challengeField = scenario.optionalString("handshake_challenge") ?: challenge,
    publicKey = scenario.optionalString("handshake_public_key") ?: SERVER_PUBLIC_KEY,
)

// ---- Client construction ------------------------------------------------------------------------

/** Resets the transport script, installs the handshake builder, then builds the client. */
private fun scenarioClient(scenario: JsonObject): ShellClient {
    ConchNetwork.reset()
    ConchNetwork.handshakeBuilder = { challenge -> scenarioHandshake(scenario, challenge) }
    return ShellClient(
        serverUrl = scenario.string("server_url"),
        apiKey = scenario.string("api_key"),
        cachedPublicKey = scenario.optionalString("cached_public_key") ?: "",
    )
}

/**
 * Scripts the endpoint queue (the caller has already reset the transport and installed the
 * handshake builder via scenarioClient): a `handshake` response first when the flow fetches the
 * public key (fetchPublicKey at the top or the lazy fetch inside encryptedPost), then the
 * endpoint response. Blank-api-key flows skip the handshake.
 */
private fun installResponses(scenario: JsonObject) {
    val needsHandshake = scenario.optionalString("api_key")?.isNotBlank() == true
    if (needsHandshake && scenario.optionalBoolean("script_handshake") != false) {
        // The handshake GET precedes the endpoint call (top-level fetch or the lazy fetch
        // inside encryptedPost); transport failures still happen AFTER it.
        ConchNetwork.queue.addLast(ConchNetwork.ScriptedResponse(dynamicHandshake = true))
    }
    val response = scenario["response"] as? JsonObject
    if (response != null) {
        ConchNetwork.queue.addLast(
            ConchNetwork.ScriptedResponse(
                code = response.optionalInt("code") ?: 200,
                body = response.optionalString("body") ?: "",
                rawBody = response.optionalString("raw_body"),
                signatureMode = response.optionalString("signature") ?: "absent",
                failure = response.optionalString("failure"),
                failureDetail = response.optionalString("failure_detail") ?: "",
            ),
        )
    }
    scenario.optionalString("transport_failure")?.let { kind ->
        ConchNetwork.queue.addLast(
            ConchNetwork.ScriptedResponse(
                failure = kind,
                failureDetail = scenario.optionalString("transport_failure_detail") ?: "",
            ),
        )
    }
}

// ---- Masking of the random request parts ---------------------------------------------------------

private fun maskHeader(key: String, value: String): String = when (key) {
    "X-Timestamp" -> "<timestamp>"
    "X-Signature" -> "<signature>"
    "X-Nonce" -> "<nonce>"
    "X-Client-Public-Key" -> "<client-public-key>"
    else -> value
}

private fun maskUrl(url: String): String =
    if (url.contains("challenge=")) {
        url.replace(Regex("challenge=[^&]*"), "challenge=<challenge>")
    } else {
        url
    }

private fun requestsJson(): JsonArray = buildJsonArray {
    ConchNetwork.requests.forEach { request ->
        val encrypted = request.headers.any { (key, _) -> key == "X-Encryption" }
        add(buildJsonObject {
            put("url", maskUrl(request.url))
            put("method", request.method)
            put("body", if (encrypted) "<encrypted>" else request.body)
            put("headers", buildJsonArray {
                request.headers.forEach { (key, value) ->
                    add(buildJsonArray {
                        add(key)
                        add(maskHeader(key, value))
                    })
                }
            })
            if (request.callTimeoutMillis == null) put("call_timeout_ms", JsonNull)
            else put("call_timeout_ms", request.callTimeoutMillis)
        })
    }
}

// ---- Ops ------------------------------------------------------------------------------------------

private fun fetchOutcome(scenario: JsonObject): JsonElement {
    ConchNetwork.reset()
    ConchNetwork.handshakeBuilder = { challenge -> scenarioHandshake(scenario, challenge) }
    when {
        scenario.optionalString("transport_failure") != null -> ConchNetwork.queue.addLast(
            ConchNetwork.ScriptedResponse(
                failure = scenario.string("transport_failure"),
                failureDetail = scenario.optionalString("transport_failure_detail") ?: "",
            ),
        )

        scenario.optionalBoolean("handshake_http_failure") == true -> ConchNetwork.queue.addLast(
            ConchNetwork.ScriptedResponse(
                code = scenario.optionalInt("handshake_failure_code") ?: 503,
                body = scenario.optionalString("handshake_failure_body") ?: "",
            ),
        )

        else -> ConchNetwork.queue.addLast(ConchNetwork.ScriptedResponse(dynamicHandshake = true))
    }
    val client = ShellClient(
        serverUrl = scenario.string("server_url"),
        apiKey = scenario.string("api_key"),
        cachedPublicKey = scenario.optionalString("cached_public_key") ?: "",
    )
    val result = runBlocking { client.fetchPublicKey() }
    return buildJsonObject {
        put("result", result)
        if (client.lastError == null) put("last_error", JsonNull)
        else put("last_error", client.lastError)
        if (client.getServerPublicKeyBase64() == null) put("server_public_key", JsonNull)
        else put("server_public_key", client.getServerPublicKeyBase64())
        put("requests", requestsJson())
    }
}

private fun prepareOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    if (scenario.optionalBoolean("run_fetch_first") == true) {
        runBlocking { client.fetchPublicKey() }
    }
    return try {
        val request = client.prepareRequest(
            scenario.string("command"),
            scenario.int("timeout_ms"),
            scenario.string("workdir"),
        )
        buildJsonObject {
            put("result", buildJsonObject {
                put("body", if (request.isEncrypted) "<encrypted>" else request.body)
                put("headers", buildJsonArray {
                    request.headers.forEach { (key, value) ->
                        add(buildJsonArray {
                            add(key)
                            add(maskHeader(key, value))
                        })
                    }
                })
                put("is_encrypted", request.isEncrypted)
                put("server_url", request.serverUrl)
            })
            put("threw", JsonNull)
            put("requests", requestsJson())
        }
    } catch (failure: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", failure.message ?: "")
            put("requests", requestsJson())
        }
    }
}

private fun decryptOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    if (scenario.optionalBoolean("run_fetch_first") == true) {
        runBlocking { client.fetchPublicKey() }
    }
    if (scenario.optionalBoolean("prepare_first") == true) {
        client.prepareRequest(scenario.string("command"), scenario.int("timeout_ms"), "")
    }
    val data = if (scenario.optionalBoolean("seal_for_session") == true) {
        val key = client.getSessionKey() ?: error("no session key to seal with")
        ShellCrypto.encrypt(key, scenario.string("plaintext").toByteArray(Charsets.UTF_8))
    } else {
        scenario.string("data")
    }
    return try {
        buildJsonObject {
            put("result", client.decryptSseData(data))
            put("threw", JsonNull)
            put("requests", requestsJson())
        }
    } catch (failure: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", failure.message ?: "")
            put("requests", requestsJson())
        }
    }
}

private fun callWithRequests(block: () -> JsonElement?): JsonElement {
    return try {
        val result = block()
        buildJsonObject {
            if (result == null) put("result", JsonNull) else put("result", result)
            put("threw", JsonNull)
            put("requests", requestsJson())
        }
    } catch (failure: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", failure.message ?: "")
            put("requests", requestsJson())
        }
    }
}

private fun fileReadOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return callWithRequests {
        val result = runBlocking {
            client.fileRead(
                scenario.string("path"),
                scenario.long("offset"),
                scenario.long("limit"),
            )
        }
        buildJsonObject {
            put("content", result.content)
            put("lines", result.lines)
            put("total_lines", result.totalLines)
            put("total_bytes", result.totalBytes)
            put("returned_bytes", result.returnedBytes)
            put("offset", result.offset)
            put("limit", result.limit)
            put("truncated", result.truncated)
            if (result.error == null) put("error", JsonNull)
            else put("error", result.error)
        }
    }
}

private fun fileWriteOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return callWithRequests {
        val error = runBlocking { client.fileWrite(scenario.string("path"), scenario.string("content")) }
        if (error == null) JsonNull else JsonPrimitive(error)
    }
}

private fun fileEditOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return callWithRequests {
        val result = runBlocking {
            client.fileEdit(
                scenario.string("path"),
                scenario.string("old_string"),
                scenario.string("new_string"),
                scenario.optionalBoolean("replace_all") ?: false,
            )
        }
        buildJsonObject {
            put("replacements", result.replacements)
            put("sha256", result.sha256)
            if (result.error == null) put("error", JsonNull)
            else put("error", result.error)
        }
    }
}

private fun fileGlobOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return try {
        val result = runBlocking {
            client.fileGlob(
                scenario.string("pattern"),
                scenario.optionalString("path") ?: "",
                scenario.optionalInt("depth"),
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
            put("requests", requestsJson())
        }
    } catch (failure: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", failure.message ?: "")
            put("requests", requestsJson())
        }
    }
}

private fun fileGrepOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return try {
        val result = runBlocking {
            client.fileGrep(
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
            put("requests", requestsJson())
        }
    } catch (failure: Exception) {
        buildJsonObject {
            put("result", JsonNull)
            put("threw", failure.message ?: "")
            put("requests", requestsJson())
        }
    }
}

private fun fileImageOutcome(scenario: JsonObject): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return callWithRequests {
        val result = runBlocking { client.fileImage(scenario.string("path")) }
        buildJsonObject {
            put("data", result.data)
            put("mimeType", result.mimeType)
            put("size", result.size)
            if (result.error == null) put("error", JsonNull)
            else put("error", result.error)
        }
    }
}

private fun jobOutcome(scenario: JsonObject, op: String): JsonElement {
    val client = scenarioClient(scenario)
    installResponses(scenario)
    return callWithRequests {
        val payload = when (op) {
            "job_start" -> runBlocking {
                client.startJob(scenario.string("command"), scenario.int("timeout_ms"), scenario.string("workdir"))
            }
            "job_list" -> runBlocking { client.listJobs() }
            "job_get" -> runBlocking { client.getJob(scenario.string("job_id")) }
            "job_stop" -> runBlocking {
                client.stopJob(
                    scenario.string("job_id"),
                    scenario.optionalLong("call_timeout_ms"),
                )
            }
            else -> runBlocking { client.acknowledgeJob(scenario.string("job_id")) }
        }
        JsonPrimitive(payload)
    }
}

// ---- JSON helpers -------------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content }

private fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalInt(name: String): Int? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toIntOrNull() }

private fun JsonObject.optionalLong(name: String): Long? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toLongOrNull() }

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    (this[name] as? JsonPrimitive)?.let { if (it == JsonNull) null else it.content.toBooleanStrictOrNull() }
