#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::gemini {

// ---------------------------------------------------------------------------------------------
// Failure shape — shared with the request layer (the golden traces record the JVM class simple
// name and the RequestFormatException message verbatim).
// ---------------------------------------------------------------------------------------------

using ProviderRequestError = openai::ProviderRequestError;

// ---------------------------------------------------------------------------------------------
// Wire models — mirror of GeminiWireModels.kt (the request half). Field order is the
// serialization order: kotlinx writes properties in declaration order with encodeDefaults=true
// and explicitNulls=false (nulls omitted, defaults kept).
// ---------------------------------------------------------------------------------------------

struct ApiThinkingConfig {
    bool include_thoughts = false;
    std::optional<std::string> thinking_level;
    std::optional<std::int32_t> thinking_budget;
};

struct ApiGenerationConfig {
    std::optional<ApiThinkingConfig> thinking_config;
    std::optional<float> temperature;
    std::optional<std::int32_t> max_output_tokens;
    std::optional<float> top_p;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
};

struct ApiToolConfig {
    bool include_server_side_tool_invocations = false;
};

struct GeminiFunctionDeclaration {
    std::string name;
    std::string description;
    std::optional<json::Value> parameters;            // typed schema object
    std::optional<json::Value> parameters_json_schema;  // external (MCP) schema, verbatim
};

struct ApiTool {
    std::optional<json::Value> code_execution;
    std::optional<json::Value> google_search;
    std::optional<std::vector<GeminiFunctionDeclaration>> function_declarations;
};

struct ApiInlineData {
    std::string mime_type;
    std::string data;
};

struct GeminiFunctionCall {
    std::optional<std::string> id;
    std::string name;
    std::optional<json::Value> args;
    std::optional<std::string> thought_signature;
};

struct GeminiFunctionResponse {
    std::optional<std::string> id;
    std::string name;
    json::Value response;
};

struct ApiExecutableCode {
    std::string language;
    std::string code;
};

struct ApiCodeExecutionResult {
    std::string outcome;
    std::string output;
};

struct ApiRequestPart {
    std::optional<std::string> text;
    std::optional<ApiInlineData> inline_data;
    std::optional<std::string> thought;
    std::optional<std::string> thought_signature;
    std::optional<ApiExecutableCode> executable_code;
    std::optional<ApiCodeExecutionResult> code_execution_result;
    std::optional<GeminiFunctionCall> function_call;
    std::optional<GeminiFunctionResponse> function_response;
};

struct ApiRequestContent {
    std::optional<std::string> role;
    std::vector<ApiRequestPart> parts;
};

struct ApiGenerateContentRequest {
    std::vector<ApiRequestContent> contents;
    std::optional<ApiRequestContent> system_instruction;
    std::optional<std::vector<ApiTool>> tools;
    std::optional<ApiToolConfig> tool_config;
    std::optional<ApiGenerationConfig> generation_config;
};

// ---------------------------------------------------------------------------------------------
// The request-side helpers that live in GeminiProvider.kt (extracted verbatim by the stand).
// ---------------------------------------------------------------------------------------------

/** buildGeminiFunctionResponse: a parsed object passes through, anything else wraps as
 *  {"result": <value>}, and a parse failure wraps the raw string. */
json::Value build_gemini_function_response(std::string_view result);

/** isGeminiToolRoundCompatible: signatures of a tool round must be attributable to the target
 *  model and provider (and present at all when the model demands them). */
bool is_gemini_tool_round_compatible(const prep::ChatMessage& message,
                                     const std::string& target_model,
                                     const std::string& target_provider_name,
                                     bool signature_required);

/** geminiModelMessageParts: code-execution history (or plain text) for a model row. */
std::vector<ApiRequestPart> gemini_model_message_parts(const prep::ChatMessage& message);

// ---------------------------------------------------------------------------------------------
// coalesceGeminiContents: consecutive contents with the same non-null role concatenate.
// ---------------------------------------------------------------------------------------------

std::vector<ApiRequestContent> coalesce_gemini_contents(
    const std::vector<ApiRequestContent>& contents);

// ---------------------------------------------------------------------------------------------
// Validation gate — ApiGenerateContentRequest.requireValidWireFormat(modelName). The provider
// string in the exception text is the hardcoded "Gemini".
// ---------------------------------------------------------------------------------------------

void require_valid_wire_format(const ApiGenerateContentRequest& request,
                               const std::string& model_name);

// ---------------------------------------------------------------------------------------------
// JSON writing — kotlinx-exact serialization with the provider's Json configuration.
// ---------------------------------------------------------------------------------------------

std::string write_api_request_content(const ApiRequestContent& content);
std::string write_api_request_content_list(const std::vector<ApiRequestContent>& contents);

// ---------------------------------------------------------------------------------------------
// The pipeline — GeminiProvider.buildApiContents + buildRequestBody and their surroundings:
// tool assembly, thinkingConfig forms, toolConfig gating, message conversion, coalescing,
// object validation, serialization, the serialized-body gate and base64 preparation.
// ---------------------------------------------------------------------------------------------

struct BuiltGeminiRequest {
    std::vector<ApiRequestContent> contents;
    std::string body_json;
    std::optional<openai::StreamingJsonBody> streaming_body;
};

BuiltGeminiRequest build_gemini_request(const std::string& provider_name,
                                        const openai::ProviderConfig& config,
                                        const std::vector<prep::ChatMessage>& messages,
                                        const std::optional<std::string>& system_prompt);

// ---------------------------------------------------------------------------------------------
// Serializable decode for the validate-mode harness — Json { ignoreUnknownKeys = true }
// .decodeFromJsonElement(ApiGenerateContentRequest.serializer(), ...) semantics.
// ---------------------------------------------------------------------------------------------

ApiGenerateContentRequest decode_api_generate_content_request(const json::Value& value);

}  // namespace agora::core::gemini
