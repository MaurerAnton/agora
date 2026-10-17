#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::openai {

// ---------------------------------------------------------------------------------------------
// Failure shapes
// ---------------------------------------------------------------------------------------------

/**
 * One failure of the request pipeline, carrying the JVM class simple name the golden traces
 * record (`core/tools/golden/openai-request`). Only the RequestFormatException gate has a
 * portable, semantic message; runtime-exception texts (JsonEncodingException and friends) belong
 * to the Kotlin runtime, so the harness records the class name alone for them.
 */
struct ProviderRequestError {
    std::string class_name;
    std::string message;
};

// ---------------------------------------------------------------------------------------------
// Wire models — mirror of app/src/main/java/.../api/ProviderRequestModels.kt. Field order is the
// serialization order: kotlinx writes properties in declaration order with encodeDefaults=true
// and explicitNulls=false (nulls omitted, defaults kept), and the traces pin that byte for byte.
// ---------------------------------------------------------------------------------------------

struct OpenAiImageUrl {
    std::string url;
};

struct OpenAiContentPart {
    std::string type;
    std::optional<std::string> text;
    std::optional<OpenAiImageUrl> image_url;
};

struct OpenAiRequestFunction {
    std::string name;
    std::string arguments;
};

struct OpenAiRequestToolCall {
    std::string id;
    std::string type = "function";
    OpenAiRequestFunction function;
};

struct OpenAiMessage {
    std::string role;
    std::optional<std::vector<OpenAiContentPart>> content;
    std::optional<std::vector<OpenAiRequestToolCall>> tool_calls;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> reasoning_content;
    // @Transient in Kotlin: carried for the Responses transport, never part of a chat body.
    std::optional<std::vector<json::Value>> response_output_items;
    std::optional<std::string> response_output_item_provider;
    std::optional<std::string> response_output_item_model;
};

struct ToolProperty {
    std::string type;
    std::string description;
    /** Nullable self-reference: optional<T> needs a complete type, shared_ptr keeps the
     *  recursive shape (values are immutable after construction, sharing is invisible). */
    std::shared_ptr<ToolProperty> items;
    std::optional<std::vector<std::pair<std::string, ToolProperty>>> properties;
    std::optional<std::vector<std::string>> required;
};

struct ToolParameters {
    std::string type = "object";
    std::vector<std::pair<std::string, ToolProperty>> properties;
    std::vector<std::string> required;
    /** An external (MCP) schema, sent verbatim; the typed fields then only summarize it. */
    std::optional<json::Value> schema;
};

struct ToolFunction {
    std::string name;
    std::string description;
    ToolParameters parameters;
};

struct ToolDefinition {
    std::string type = "function";
    ToolFunction function;
};

/** DeepSeek thinking toggle: `{"thinking": {"type": "enabled" | "disabled"}}`. */
struct OpenAiThinking {
    std::string type;
};

struct OpenAiReasoning {
    std::optional<std::string> effort;
    std::optional<std::string> summary;
    std::optional<std::int32_t> max_tokens;
    std::optional<bool> enabled;
};

struct OpenAiPlugin {
    std::string id;
};

struct OpenAiStreamOptions {
    bool include_usage = true;
};

struct OpenAiChatRequest {
    std::string model;
    std::vector<OpenAiMessage> messages;
    bool stream = true;
    std::optional<OpenAiStreamOptions> stream_options;
    std::optional<std::vector<ToolDefinition>> tools;
    std::optional<std::string> reasoning_effort;
    std::optional<OpenAiThinking> thinking;
    std::optional<bool> enable_thinking;
    std::optional<std::int32_t> thinking_budget;
    std::optional<OpenAiReasoning> reasoning;
    std::optional<bool> include_reasoning;
    std::optional<std::vector<OpenAiPlugin>> plugins;
    std::optional<std::string> service_tier;
    std::optional<float> temperature;
    std::optional<std::int32_t> max_tokens;
    std::optional<float> top_p;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<std::string> prompt_cache_key;
};

// ---------------------------------------------------------------------------------------------
// Conversation input — the ChatMessage subset the request pipeline reads.
// ---------------------------------------------------------------------------------------------

enum class Participant { User, Model, Error };

struct MessageSegment {
    std::string type;
    std::string content;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_args;
    std::optional<std::string> tool_result;
    std::optional<std::string> tool_call_id;
    std::vector<json::Value> response_output_items;
    std::optional<std::string> response_output_item_provider;
};

struct ToolCallData {
    std::string tool_name;
    std::string arguments;
    std::string result;
    std::optional<std::string> tool_call_id;
    std::vector<json::Value> response_output_items;
    std::optional<std::string> response_output_item_provider;
};

struct ChatMessage {
    std::string id;
    std::string text;
    std::vector<std::string> images;
    Participant participant = Participant::User;
    std::optional<std::string> model_name;
    std::optional<std::vector<MessageSegment>> segments;
    std::optional<ToolCallData> tool_call;
};

struct ProviderRequestInput {
    std::vector<ChatMessage> messages;
    std::optional<std::string> system_prompt;
};

/** The ProviderConfig fields the chat pipeline reads; the rest (headers, retry, context window)
 *  belong to the transport slices. */
struct ProviderConfig {
    std::string api_key;
    std::string model_id;
    bool thinking_enabled = true;
    std::string thinking_level = "medium";
    bool thinking_budget_enabled = false;
    std::int32_t thinking_budget_tokens = 4096;
    std::optional<std::vector<ToolDefinition>> tools;
    bool include_images = true;
    bool include_assistant_reasoning = false;
    std::optional<float> temperature;
    std::optional<std::int32_t> max_tokens;
    std::optional<float> top_p;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<std::string> prompt_cache_key;
    std::optional<std::string> open_ai_service_tier;
    bool google_search_enabled = false;
    /** Custom-relay endpoint as the provider resolves it (config.baseUrl ?? defaultBaseUrl). */
    std::optional<std::string> base_url;
    /** Anthropic prompt-cache controls (AnthropicProvider reads them in buildRequestBody). */
    bool anthropic_cache_enabled = true;
    std::string anthropic_cache_ttl = "1h";
};

// ---------------------------------------------------------------------------------------------
// Thinking capability layer — model/ModelThinkingCapability*.kt + model/ThinkingControl.kt.
// ---------------------------------------------------------------------------------------------

enum class ThinkingProviderFamily {
    Anthropic,
    OpenAi,
    OpenAiCompatible,
    OpenRouter,
    DeepSeek,
    Qwen,
    Groq,
    Gemini,
    Ollama,
    Local,
};

/** ThinkingLevels.effortValues: the ordered selector set every capability draws from. */
const std::vector<std::string>& effort_values();

/** ThinkingLevels.normalizeEffortValue: trim/lowercase, legacy aliases, blank -> "medium". */
std::string normalize_effort_level(const std::string& raw);

/** ThinkingLevels.effortForIndex: the ordered selector, defaulting past the end. */
std::string effort_for_index(std::size_t index);

/** ThinkingLevels.indexForEffort: normalized effort position, clamped to zero. */
std::size_t index_for_effort(const std::string& effort);

/** ThinkingLevels.legacyBudgetTokens: the "budget:<n>" migration helper. */
std::optional<std::int32_t> legacy_budget_tokens(const std::string& raw);

struct ModelThinkingCapability {
    bool can_disable_thinking = true;
    std::vector<std::string> supported_efforts = [] { return effort_values(); }();
    bool supports_thinking_budget = true;
    std::int32_t min_budget_tokens = 1;
    std::optional<std::int32_t> max_budget_tokens;
    bool supports_effort_with_budget = true;
    bool supports_sampling_params = true;

    bool supports_effort() const { return !supported_efforts.empty(); }
    std::optional<std::string> lowest_effort() const;
    /** Nearest accepted effort; equal distances resolve upward; empty set -> nullopt. */
    std::optional<std::string> nearest_effort(const std::string& effort) const;
    std::int32_t clamp_budget(std::int32_t tokens) const;

    /** The permissive superset for undocumented models. */
    static const ModelThinkingCapability& permissive();
};

struct ResolvedThinking {
    bool enabled = true;
    std::optional<std::string> effort;
    std::optional<std::int32_t> budget_tokens;
    ModelThinkingCapability capability;
    bool disabled() const { return !enabled; }
};

/** ModelThinkingCapabilities.familyFor: persisted protocol wins over the display name. */
ThinkingProviderFamily family_for(const std::optional<std::string>& provider_name,
                                  const std::optional<std::string>& custom_protocol_wire_value =
                                      std::nullopt);

const ModelThinkingCapability& family_default(ThinkingProviderFamily family);
ModelThinkingCapability capability_for_model(ThinkingProviderFamily family,
                                             const std::string& model_id);

/** ModelThinkingCapabilities.forProvider: protocol/display-name family plus model refinement. */
ModelThinkingCapability capability_for_provider(
    const std::optional<std::string>& provider_name, const std::string& model_id,
    const std::optional<std::string>& custom_protocol_wire_value = std::nullopt);

ResolvedThinking resolve_thinking(const ModelThinkingCapability& capability,
                                  bool requested_enabled,
                                  const std::string& requested_effort,
                                  bool requested_budget_enabled,
                                  std::int32_t requested_budget_tokens);

/** ProviderConfig.resolvedThinking: the request's settings met with the model's capability. */
ResolvedThinking resolved_thinking(const ProviderConfig& config, ThinkingProviderFamily family);

// ---------------------------------------------------------------------------------------------
// Provider hooks — the six `customizeRequest` bodies (openai/OpenAiThinkingHooks.kt).
// ---------------------------------------------------------------------------------------------

OpenAiChatRequest with_open_ai_thinking(OpenAiChatRequest request,
                                        const ResolvedThinking& resolved);
OpenAiChatRequest with_deep_seek_thinking(OpenAiChatRequest request,
                                          const ResolvedThinking& resolved);
OpenAiChatRequest with_groq_thinking(OpenAiChatRequest request, const ResolvedThinking& resolved,
                                     bool thinking_enabled);
OpenAiChatRequest with_qwen_thinking(OpenAiChatRequest request, const ResolvedThinking& resolved);
OpenAiChatRequest with_open_router_reasoning(OpenAiChatRequest request,
                                             const ResolvedThinking& resolved,
                                             bool google_search_enabled);
OpenAiChatRequest with_custom_open_ai_thinking(OpenAiChatRequest request,
                                               const ResolvedThinking& resolved,
                                               std::string_view default_base_url);

bool is_cheapest_inference_endpoint(std::string_view default_base_url);
std::string cheapest_inference_effort(bool disabled, const std::optional<std::string>& effort);

// ---------------------------------------------------------------------------------------------
// Tool-call identity — util/MessageConverter.kt::buildToolCallId (SHA-256 based).
// ---------------------------------------------------------------------------------------------

std::string build_tool_call_id(std::string_view tool_name, std::string_view arguments,
                               std::string_view prefix = "call_");

// ---------------------------------------------------------------------------------------------
// JSON writing — kotlinx-exact serialization of the wire models.
// ---------------------------------------------------------------------------------------------

/** Kotlin Float.toString() (JVM FloatingDecimal): shortest round-trip, plain in [1e-3, 1e7),
 *  computerized scientific elsewhere ("1.0E-7", "3.4028235E38", "-0.0"). */
std::string format_kotlin_float(float value);

/** java.util.Base64.getEncoder(): RFC 4648 with '=' padding. */
std::string encode_base64(std::string_view bytes);

/** ListSerializer(OpenAiMessage.serializer()) output — the trace's apiMessages line. */
std::string write_open_ai_message_list(const std::vector<OpenAiMessage>& messages);

std::string write_open_ai_message(const OpenAiMessage& message);

/** OpenAiChatRequest.serializer(); non-finite floats throw JsonEncodingException. */
std::string write_open_ai_chat_request(const OpenAiChatRequest& request);

/** `Json { ignoreUnknownKeys = true }.decodeFromJsonElement` for wire-form (validate mode). */
OpenAiChatRequest decode_open_ai_chat_request(const json::Value& value);

// ---------------------------------------------------------------------------------------------
// Base64 attachment projection — util/Base64FileRegistry.kt + util/StreamingJsonRequestBody.kt.
// ---------------------------------------------------------------------------------------------

struct Base64FilePart {
    std::string placeholder;
    std::string path;  // canonical file path
    std::int64_t byte_count = 0;
    std::int64_t last_modified = 0;  // file mtime, nanoseconds since the epoch
    std::array<std::uint8_t, 32> sha256{};

    /** "[STREAMED_BASE64:<bytes>_BYTES]" — the diagnostic projection of the placeholder. */
    std::string diagnostic_marker() const;
    bool metadata_matches() const;
};

struct StreamingJsonBody {
    std::vector<std::string> segments;  // one more than parts: segment[i], b64(part[i]), ...
    std::vector<Base64FilePart> parts;
    std::string diagnostic_json;
    std::int64_t content_length = 0;

    /** The exact bytes the wire body produces once every placeholder is replaced by its base64:
     *  the substitution ReplayableJsonRequestBody.writeTo performs, without the transport checks.
     *  This is what the golden traces pin. */
    std::string resolved_bytes() const;

    /** Transport form: re-verifies each attachment's metadata and SHA-256 on every write, so a
     *  changed file fails instead of corrupting the replayed request. Throws
     *  ProviderRequestError{"IOException", ...}. */
    std::string write_bytes() const;
};

class Base64FileRegistry {
public:
    /** Projects one file; returns its placeholder, or nullopt when the file is not a stable
     *  regular file (missing path, directory, changed mid-hash). */
    std::optional<std::string> register_file(const std::string& path);

    /** Splits the serialized body around every registered placeholder; nullopt when nothing was
     *  registered. Throws ProviderRequestError{"IllegalArgumentException", ...} when a
     *  placeholder is missing, duplicated or overlapping. */
    std::optional<StreamingJsonBody> prepare(const std::string& template_json) const;

private:
    std::vector<Base64FilePart> files_;
};

// ---------------------------------------------------------------------------------------------
// Message conversion — util/MessageConverter.kt.
// ---------------------------------------------------------------------------------------------

std::string image_mime_type(std::string_view image_path);

std::vector<OpenAiMessage> convert_to_open_ai_messages(
    const std::vector<ChatMessage>& messages,
    const std::optional<std::string>& system_prompt,
    bool include_images,
    Base64FileRegistry& base64_files,
    bool forward_assistant_reasoning);

// ---------------------------------------------------------------------------------------------
// Wire validation gates — openai/OpenAiChatRequestValidator.kt + util/RequestFormatValidation.kt.
// ---------------------------------------------------------------------------------------------

void require_valid_wire_format(const OpenAiChatRequest& request, const std::string& provider);

/** Field lists mirror Kotlin `Set` arguments: insertion-ordered, duplicate-free. */
void require_valid_serialized_request(const std::string& provider, std::string_view body,
                                      const std::vector<std::string>& required_string_fields,
                                      const std::vector<std::string>& required_array_fields);

// ---------------------------------------------------------------------------------------------
// The pipeline — openai/OpenAiChatRequestFactory.kt: conversation -> wire messages -> typed
// request -> customize hook -> object validation -> serialization -> serialized-body gate ->
// base64 body preparation. Every step order and wire shape is pinned by the golden traces.
// ---------------------------------------------------------------------------------------------

enum class ChatProviderKind { OpenAi, DeepSeek, Groq, Qwen, OpenRouter, Custom };

struct BuiltChatRequest {
    std::vector<OpenAiMessage> api_messages;
    std::string body_json;
    std::optional<StreamingJsonBody> streaming_body;
};

/** `provider_name` is the identity the format gates put into their exception text (the provider
 *  display name), `kind` picks the customize hook — the two travel together in every provider. */
BuiltChatRequest build_chat_request(ChatProviderKind kind, const std::string& provider_name,
                                     const ProviderConfig& config,
                                     const ProviderRequestInput& input);

}  // namespace agora::core::openai
