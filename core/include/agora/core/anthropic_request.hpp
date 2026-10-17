#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::anthropic {

// ---------------------------------------------------------------------------------------------
// Failure shape — the request layer's own ProviderRequestError (shared with the OpenAI request
// module); the golden traces record the JVM class simple name and the RequestFormatException
// message verbatim.
// ---------------------------------------------------------------------------------------------

using ProviderRequestError = openai::ProviderRequestError;

// ---------------------------------------------------------------------------------------------
// Wire models — mirror of the @Serializable classes in AnthropicProvider.kt. Field order is the
// serialization order: kotlinx writes properties in declaration order with encodeDefaults=true
// and explicitNulls=false (nulls omitted, defaults kept), and the traces pin that byte for byte.
// ---------------------------------------------------------------------------------------------

struct AnthropicCacheControl {
    std::string type = "ephemeral";
    std::string ttl = "1h";
};

struct AnthropicTool {
    std::string name;
    std::string description;
    json::Value input_schema;  // ToolSchemaJson.of output (a JsonObject value)
};

struct AnthropicThinking {
    std::string type = "enabled";  // "enabled" | "adaptive" | "disabled"
    std::optional<std::int32_t> budget_tokens;
    std::optional<std::string> display;
};

struct AnthropicOutputConfig {
    std::string effort;
};

struct AnthropicImageSource {
    std::string type = "base64";
    std::string media_type;
    std::string data;  // Base64FileRegistry placeholder
};

struct AnthropicContentPart {
    std::string type;  // "text" | "image" | "thinking" | "tool_use" | "tool_result"
    std::optional<std::string> text;
    std::optional<std::string> thinking;
    std::optional<std::string> signature;
    std::optional<AnthropicImageSource> source;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<json::Value> input;     // tool_use argument object
    std::optional<std::string> tool_use_id;
    std::optional<std::string> content;   // tool_result payload
};

struct AnthropicMessage {
    std::string role;  // "user" | "assistant"
    std::vector<AnthropicContentPart> content;
};

struct AnthropicRequest {
    std::string model;
    std::vector<AnthropicMessage> messages;
    std::optional<std::string> system;
    std::int32_t max_tokens = 4096;
    bool stream = true;
    std::optional<AnthropicThinking> thinking;
    std::optional<AnthropicOutputConfig> output_config;
    std::optional<std::vector<AnthropicTool>> tools;
    std::optional<AnthropicCacheControl> cache_control;
    std::optional<float> temperature;
    std::optional<float> top_p;
};

// ---------------------------------------------------------------------------------------------
// Conversation input — the prepared context the provider's buildRequestBody consumes. Messages
// are prep::ChatMessage: the Anthropic builder reads thought signatures (protocol state), which
// only that shape carries.
// ---------------------------------------------------------------------------------------------

struct ProviderRequestInput {
    std::vector<prep::ChatMessage> messages;
    std::optional<std::string> system_prompt;
};

// ---------------------------------------------------------------------------------------------
// AnthropicOutputLimits.kt — the documented max_tokens ceilings of Claude models.
// ---------------------------------------------------------------------------------------------

std::optional<std::int32_t> max_output_tokens(std::string_view model_id);

// ---------------------------------------------------------------------------------------------
// Message conversion — the provider's private helpers (AnthropicProvider.kt), pinned through the
// factory traces.
// ---------------------------------------------------------------------------------------------

/** buildToolUseBlock: safe id fallback via buildToolCallId, args parsed to an object (or {}). */
AnthropicContentPart build_tool_use_block(const std::optional<std::string>& id,
                                          const std::optional<std::string>& name,
                                          const std::optional<std::string>& args);

/** buildToolResultBlocks: tool segments, or the legacy toolCall shape, or nothing. */
std::vector<AnthropicContentPart> build_tool_result_blocks(const prep::ChatMessage& msg);

/** The assistant tool_use turn, replaying every signed thinking block whose signature matches. */
AnthropicMessage build_assistant_tool_use(const prep::ChatMessage& msg,
                                          const std::string& target_model,
                                          const std::string& provider_name);

/** A normal turn: user images (registered as placeholders), non-blank text, "Continue" fallback. */
AnthropicMessage build_normal_message(const prep::ChatMessage& msg,
                                      openai::Base64FileRegistry& base64_files);

/** coalesceAnthropicMessages: consecutive same-role turns concatenate their content. */
std::vector<AnthropicMessage> coalesce_anthropic_messages(
    const std::vector<AnthropicMessage>& messages);

// ---------------------------------------------------------------------------------------------
// Validation gate — AnthropicRequest.requireValidWireFormat. The provider string is the
// hardcoded "Anthropic" the Kotlin extension uses in its exception text.
// ---------------------------------------------------------------------------------------------

void require_valid_wire_format(const AnthropicRequest& request);

// ---------------------------------------------------------------------------------------------
// JSON writing — kotlinx-exact serialization with the provider's Json configuration.
// ---------------------------------------------------------------------------------------------

std::string write_anthropic_message(const AnthropicMessage& message);
std::string write_anthropic_message_list(const std::vector<AnthropicMessage>& messages);

// ---------------------------------------------------------------------------------------------
// The pipeline — AnthropicProvider.buildRequestBody and its surrounding steps: thinking
// resolution, ToolSchemaJson conversion, tool-round adaptation, message conversion, coalescing,
// max_tokens defaults, sampling gates, object validation, serialization, the serialized-body
// gate and base64 preparation. Every step order and wire shape is pinned by the golden traces.
// ---------------------------------------------------------------------------------------------

struct BuiltAnthropicRequest {
    std::vector<AnthropicMessage> api_messages;
    std::string body_json;
    std::optional<openai::StreamingJsonBody> streaming_body;
};

BuiltAnthropicRequest build_anthropic_request(const std::string& provider_name,
                                              const openai::ProviderConfig& config,
                                              const ProviderRequestInput& input);

// ---------------------------------------------------------------------------------------------
// Serializable decode for the validate-mode harness — Json { ignoreUnknownKeys = true }
// .decodeFromJsonElement(AnthropicRequest.serializer(), ...) semantics.
// ---------------------------------------------------------------------------------------------

AnthropicRequest decode_anthropic_request(const json::Value& value);

}  // namespace agora::core::anthropic
