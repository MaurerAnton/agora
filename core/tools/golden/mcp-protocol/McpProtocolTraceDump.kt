// Golden trace driver for the MCP protocol core, used to pin the C++ port.
//
// McpModels.kt compiles VERBATIM from the app tree; extract_mcp_pieces.py copies the private
// schema projections, the protocol constants, the version classifier and parseCallPayload
// verbatim (visibility widened). The JSON-RPC response validation, the negotiation candidate
// order and the tools/list page mapping are inline in transport-coupled functions — the driver
// transcribes them line for line (documented in README.md).
//
// Scenario format: see README.md in this directory. Build/run: see run_golden.sh.
package mcp.protocol.golden

import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.mcp.McpToolDescriptor
import com.newoether.agora.mcp.McpRemoteTool
import com.newoether.agora.mcp.asObjectOrNull
import com.newoether.agora.mcp.isUnsupportedMcpProtocolVersion
import com.newoether.agora.mcp.parseCallPayload
import com.newoether.agora.mcp.publicMcpToolName
import com.newoether.agora.mcp.LEGACY_SSE_PROTOCOL_VERSION
import com.newoether.agora.mcp.STREAMABLE_HTTP_PROTOCOL_VERSIONS
import com.newoether.agora.mcp.SUPPORTED_PROTOCOL_VERSIONS
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.put
import java.io.File
import java.io.IOException

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

private fun orNull(value: String?): String = value?.let { esc(it) } ?: "-"

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "toolName",
    val serverId: String = "srv-1",
    val serverName: String = "Server One",
    val remoteName: String = "search",
    val description: String = "",
    val inputSchema: JsonObject? = null,
    val result: JsonObject? = null,
    val method: String = "tools/call",
    val id: Long = 1,
    val response: JsonObject? = null,
    val transportType: String = "STREAMABLE_HTTP",
    val currentVersion: String = "2025-11-25",
    val messages: List<String> = emptyList(),
)

/** Transcription of requestLocked's response validation (lines 225-237, transport removed). */
private fun validateRpcResponse(method: String, id: Long, response: JsonObject?): JsonObject {
    response ?: throw IOException("MCP $method returned an empty response")
    val responseId = (response["id"] as? JsonPrimitive)?.contentOrNull
    if (responseId != id.toString()) {
        throw IOException("MCP $method returned mismatched response id")
    }
    response["error"]?.takeUnless { it is JsonNull }?.let { error ->
        val errorObj = error.asObjectOrNull()
        val message = (errorObj?.get("message") as? JsonPrimitive)?.contentOrNull
            ?: error.toString()
        throw IOException("MCP $method failed: $message")
    }
    return response["result"]?.asObjectOrNull()
        ?: throw IOException("MCP $method returned no result")
}

/** Transcription of ensureInitializedLocked's candidate order (lines 132-137). */
private fun negotiationCandidates(transportType: String, protocolVersion: String): List<String> =
    if (transportType == "STREAMABLE_HTTP") {
        val versions = STREAMABLE_HTTP_PROTOCOL_VERSIONS
        versions.dropWhile { it != protocolVersion }.ifEmpty { versions }
    } else {
        listOf(LEGACY_SSE_PROTOCOL_VERSION)
    }

/** Transcription of listTools' page mapping (lines 79-101, transport removed). */
private fun toolsPage(result: JsonObject): Pair<List<McpRemoteTool>, String?> {
    val page = result["tools"] as? JsonArray
        ?: throw IOException("MCP tools/list returned no tools array")
    val tools = mutableListOf<McpRemoteTool>()
    page.forEach { element ->
        val obj = element.asObjectOrNull() ?: return@forEach
        val name = (obj["name"] as? JsonPrimitive)?.contentOrNull
            ?.takeIf(String::isNotBlank)
            ?: return@forEach
        tools += McpRemoteTool(
            name = name,
            description = (obj["description"] as? JsonPrimitive)?.contentOrNull.orEmpty(),
            inputSchema = obj["inputSchema"]?.asObjectOrNull()
                ?: buildJsonObject {
                    put("type", "object")
                    put("properties", buildJsonObject {})
                },
        )
    }
    val cursor = (result["nextCursor"] as? JsonPrimitive)?.contentOrNull
        ?.takeIf(String::isNotBlank)
    return tools to cursor
}

/** Transcription of initializeLocked's result validation (lines 176-180). */
private fun negotiatedVersion(result: JsonObject): String {
    val negotiated = (result["protocolVersion"] as? JsonPrimitive)?.contentOrNull
        ?: throw IOException("MCP initialize returned no protocolVersion")
    if (negotiated !in SUPPORTED_PROTOCOL_VERSIONS) {
        throw IOException("Unsupported MCP protocol version: $negotiated")
    }
    return negotiated
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: McpProtocolTraceDump <scenario.json> <unusedRoot>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    try {
        when (scenario.mode) {
            "toolName" -> {
                line("name", esc(publicMcpToolName(scenario.serverId, scenario.remoteName)))
            }
            "toolDefinition" -> {
                val remote = McpRemoteTool(
                    name = scenario.remoteName,
                    description = scenario.description,
                    inputSchema = scenario.inputSchema ?: buildJsonObject {
                        put("type", "object")
                        put("properties", buildJsonObject {})
                    },
                )
                val descriptor = McpToolDescriptor(
                    publicName = publicMcpToolName(scenario.serverId, scenario.remoteName),
                    serverId = scenario.serverId,
                    serverName = scenario.serverName,
                    remote = remote,
                )
                val definition = descriptor.asToolDefinition()
                line("definition", esc(traceJson.encodeToString(ToolDefinition.serializer(), definition)))
                line("schema", orNull(definition.function.parameters.schema?.toString()))
            }
            "callPayload" -> {
                val payload = parseCallPayload(scenario.result ?: buildJsonObject {})
                for ((index, text) in payload.textParts.withIndex()) {
                    line("text", index.toString(), esc(text))
                }
                for ((index, image) in payload.images.withIndex()) {
                    line("image", index.toString(), esc(image.data), esc(image.mimeType))
                }
                val structured: JsonElement? = payload.structuredContent
                line("structured", if (structured == null) "-" else esc(structured.toString()))
                line("isError", if (payload.isError) "1" else "0")
            }
            "toolsPage" -> {
                val (tools, cursor) = toolsPage(scenario.result ?: buildJsonObject {})
                for ((index, tool) in tools.withIndex()) {
                    line("tool", index.toString(), esc(tool.name), esc(tool.description),
                        esc(tool.inputSchema.toString()))
                }
                line("cursor", orNull(cursor))
            }
            "rpcResponse" -> {
                val result = validateRpcResponse(scenario.method, scenario.id, scenario.response)
                line("result", esc(result.toString()))
            }
            "negotiation" -> {
                line("candidates", negotiationCandidates(scenario.transportType, scenario.currentVersion).joinToString(","))
            }
            "unsupportedVersion" -> {
                var error: Throwable = IOException(scenario.messages.lastOrNull() ?: "")
                for (index in scenario.messages.dropLast(1).indices.reversed()) {
                    error = IOException(scenario.messages[index], error)
                }
                line("unsupported", if (isUnsupportedMcpProtocolVersion(error)) "1" else "0")
            }
            "initialize" -> {
                val negotiated = negotiatedVersion(scenario.result ?: buildJsonObject {})
                line("negotiated", esc(negotiated))
                line("sameAsRequested",
                    if (negotiated == scenario.currentVersion) "1" else "0")
            }
            else -> error("unknown mode: ${scenario.mode}")
        }
    } catch (error: Exception) {
        line("error", error.javaClass.simpleName ?: "unknown", orNull(error.message))
    }
    print(out)
}