package com.newoether.agora.tool

import android.app.Application
import com.newoether.agora.api.HttpClient
import com.newoether.agora.api.ProviderDefaults
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.ToolFunction
import com.newoether.agora.api.ToolParameters
import com.newoether.agora.api.ToolProperty
import com.newoether.agora.util.Constants
import com.newoether.agora.util.DebugLog
import com.newoether.agora.viewmodel.GenerationContext
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

/**
 * Tool that generates images through an OpenAI-compatible `/images/generations` endpoint,
 * or through OpenRouter's dedicated `/images` endpoint when the selected image model
 * resolves to an `openrouter.ai` base URL (e.g. `openai/gpt-image-2.5-sunburst`,
 * whose `output_modalities` is image-only and is not served over `/chat/completions`).
 * Successful bytes are persisted by [ToolImageStore] and returned on the owning tool result.
 */
class ImageGenToolProvider(private val app: Application) : ToolProvider {

    private val imageStore = ToolImageStore(app)

    override fun definitions(ctx: GenerationContext): List<ToolDefinition> {
        if (!ctx.imageGenEnabled) return emptyList()
        return listOf(
            ToolDefinition(function = ToolFunction(
                name = "generate_image",
                description = "Generate an image from a text prompt. The generated image is shown to the user automatically — do NOT attempt to embed or describe the raw image data. Use this whenever the user asks to create, draw, paint, or generate a picture.",
                parameters = ToolParameters(
                    properties = mapOf(
                        "prompt" to ToolProperty("string", "A detailed description of the image to generate."),
                        "size" to ToolProperty("string", "Optional image size, e.g. 1024x1024, 1024x1536, or 1536x1024.")
                    ),
                    required = listOf("prompt")
                )
            ))
        )
    }

    override fun handles(name: String): Boolean = name == "generate_image"

    override suspend fun execute(
        name: String,
        arguments: String,
        ctx: GenerationContext,
    ): String = executeResult(name, arguments, ctx).text

    override fun executeEvents(
        name: String,
        arguments: String,
        ctx: GenerationContext,
    ): Flow<ToolExecutionEvent> = flow {
        emit(ToolExecutionEvent.Completed(executeResult(name, arguments, ctx)))
    }

    private suspend fun executeResult(
        name: String,
        arguments: String,
        ctx: GenerationContext,
    ): ToolExecutionResult = withContext(Dispatchers.IO) {
        val textAndImage = executeOffMain(name, arguments, ctx)
        ToolExecutionResult(
            text = textAndImage.first,
            images = listOfNotNull(textAndImage.second),
            isError = textAndImage.second == null,
        )
    }

    private suspend fun executeOffMain(
        name: String,
        arguments: String,
        ctx: GenerationContext,
    ): Pair<String, com.newoether.agora.model.ToolImageAttachment?> {
        ctx.conversationId?.takeIf { it.isNotBlank() }
            ?: return err("missing_conversation", "Image generation requires a conversation.") to null
        val argsStr = arguments.ifBlank { "{}" }
        val args = try {
            Json.decodeFromString<Map<String, kotlinx.serialization.json.JsonElement>>(argsStr)
        } catch (_: Exception) { emptyMap() }
        val prompt = (args["prompt"] as? JsonPrimitive)?.content
            ?: return err("no_prompt", null) to null
        val size = (args["size"] as? JsonPrimitive)?.content?.takeIf { it.isNotBlank() }
            ?: ctx.imageGenSize.ifBlank { "1024x1024" }

        val apiKey = ctx.imageGenApiKey
        if (apiKey.isBlank()) return err("no_api_key", null) to null
        val baseUrl = ctx.imageGenBaseUrl.ifBlank { ProviderDefaults.OPENAI_BASE_URL }.trimEnd('/')
        val model = ctx.imageGenModel.ifBlank { "gpt-image-1" }

        return withContext(Dispatchers.IO) {
            try {
                // OpenRouter does not serve `/images/generations`. Its Image API is
                // `POST {base}/images` with `{model, prompt, aspect_ratio, n}` and
                // `{data:[{b64_json, media_type}]}`. `size` ("1024x1024") is not a
                // supported parameter there (checked against
                // `/api/v1/images/models/openai/gpt-image-2.5-sunburst/endpoints`),
                // so it is converted to the nearest supported aspect ratio.
                val openRouter = isOpenRouterEndpoint(baseUrl)
                val endpoint = if (openRouter) "$baseUrl/images" else "$baseUrl/images/generations"
                val aspectRatio = if (openRouter) aspectRatioForSize(size) else null
                val body = buildJsonObject {
                    put("model", model)
                    put("prompt", prompt)
                    if (openRouter) {
                        put("aspect_ratio", aspectRatio ?: "auto")
                    } else {
                        put("size", size)
                    }
                    put("n", 1)
                }.toString()
                val headers = buildMap {
                    put("Authorization", "Bearer $apiKey")
                    if (openRouter) {
                        put("HTTP-Referer", "https://github.com/newo-ether/Agora")
                        put("X-Title", "Agora")
                    }
                }
                val response = HttpClient.post(
                    endpoint,
                    body,
                    headers,
                    callTimeoutMillis = Constants.IMAGE_GENERATION_TIMEOUT_MS,
                    readTimeoutMillis = Constants.IMAGE_GENERATION_TIMEOUT_MS,
                ) ?: return@withContext err("no_response", null) to null

                val json = Json.decodeFromString<Map<String, kotlinx.serialization.json.JsonElement>>(response)
                val first = json["data"]?.jsonArray?.firstOrNull()?.jsonObject
                    ?: return@withContext err("no_image", "The endpoint returned no image data.") to null

                val bytes: ByteArray = run {
                    val b64 = (first["b64_json"] as? JsonPrimitive)?.content
                    if (!b64.isNullOrBlank()) {
                        android.util.Base64.decode(b64, android.util.Base64.DEFAULT)
                    } else {
                        val url = (first["url"] as? JsonPrimitive)?.content
                            ?: return@withContext err(
                                "no_image",
                                "No b64_json or url in the response.",
                            ) to null
                        HttpClient.getBytes(
                            url,
                            callTimeoutMillis = Constants.IMAGE_GENERATION_TIMEOUT_MS,
                            readTimeoutMillis = Constants.IMAGE_GENERATION_TIMEOUT_MS,
                        ) ?: return@withContext err("download_failed", null) to null
                    }
                }

                val attachment = imageStore.persistGeneratedBytes(
                    bytes = bytes,
                    filePrefix = "generated_image",
                )
                buildJsonObject {
                    put("type", "image_generation")
                    put("status", "ok")
                    put("size", size)
                    put("model", model)
                    if (aspectRatio != null) put("aspect_ratio", aspectRatio)
                }.toString() to attachment
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                DebugLog.e("ImageGenTool", "generate_image failed", e)
                err("generation_error", e.message) to null
            }
        }
    }

    private fun err(code: String, message: String?): String = buildJsonObject {
        put("type", "image_generation")
        put("error", code)
        if (!message.isNullOrBlank()) put("message", message)
    }.toString()
}

/**
 * True when [baseUrl] targets OpenRouter (`https://openrouter.ai/api/v1` or a custom
 * provider override pointing there). Those endpoints serve image models such as
 * `openai/gpt-image-2.5-sunburst` over `POST /images`, not OpenAI's `/images/generations`.
 */
internal fun isOpenRouterEndpoint(baseUrl: String): Boolean =
    baseUrl.contains("openrouter.ai", ignoreCase = true)

/**
 * Maps an image-tool size (`"1024x1024"`, the `Settings → Image Generation` default) to the
 * nearest aspect ratio accepted by OpenRouter image endpoints. `gpt-image-2.5-sunburst`
 * accepts `1:1, 3:2, 2:3, 4:3, 3:4, 16:9, 9:16, 21:9, auto`; unparsable input falls back
 * to `auto` so the provider chooses instead of failing with 400.
 */
internal fun aspectRatioForSize(size: String): String {
    val match = Regex("""(\d+)\s*[xX×]\s*(\d+)""").find(size.trim())
        ?: return "auto"
    val w = match.groupValues[1].toIntOrNull() ?: return "auto"
    val h = match.groupValues[2].toIntOrNull() ?: return "auto"
    if (w <= 0 || h <= 0) return "auto"
    val ratio = w.toDouble() / h.toDouble()
    val candidates = listOf(
        "1:1" to 1.0,
        "4:3" to 4.0 / 3.0,
        "3:4" to 3.0 / 4.0,
        "3:2" to 3.0 / 2.0,
        "2:3" to 2.0 / 3.0,
        "16:9" to 16.0 / 9.0,
        "9:16" to 9.0 / 16.0,
        "21:9" to 21.0 / 9.0,
    )
    val logRatio = kotlin.math.ln(ratio)
    return candidates.minByOrNull {
        kotlin.math.abs(kotlin.math.ln(it.second) - logRatio)
    }?.first ?: "auto"
}
