#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/stream_normalization.hpp"
#include "agora/core/token_usage.hpp"

namespace agora::core::ollama {

namespace sn = agora::core::stream_normalization;

// ---------------------------------------------------------------------------------------------
// Failure shapes — shared with the request layer; the golden traces record the JVM class simple
// name and the RequestFormatException message verbatim.
// ---------------------------------------------------------------------------------------------

using ProviderRequestError = openai::ProviderRequestError;

/** Decode failure carrier for the stream driver (the cause text is not pinned by the traces). */
struct DecodeFailure {
    std::string message;
};

// ---------------------------------------------------------------------------------------------
// Tool-call wire shapes — OpenAiStreamModels.kt (the classes Ollama's NDJSON and wire carry).
// ---------------------------------------------------------------------------------------------

struct FunctionCall {
    std::optional<std::string> name;
    std::optional<json::Value> arguments;
};

struct ToolCall {
    std::optional<std::int32_t> index;
    std::optional<std::string> id;
    std::optional<std::string> type;
    std::optional<FunctionCall> function;
};

// ---------------------------------------------------------------------------------------------
// Wire models — OllamaProvider.kt. Field order is the serialization order (encodeDefaults=true,
// explicitNulls=false): `content` always serializes (its default is ""), nulls are omitted.
// ---------------------------------------------------------------------------------------------

struct OllamaMessage {
    std::string role;
    std::string content;
    std::optional<std::string> thinking;
    std::optional<std::vector<std::string>> images;
    std::optional<std::string> tool_name;
    std::optional<std::vector<ToolCall>> tool_calls;
};

struct OllamaChatRequest {
    std::string model;
    std::vector<OllamaMessage> messages;
    bool stream = true;
    std::optional<json::Value> options;
    std::optional<std::vector<openai::ToolDefinition>> tools;
    std::optional<json::Value> think;
};

struct OllamaStreamResponse {
    std::optional<std::string> model;
    std::optional<OllamaMessage> message;
    bool done = false;
    std::optional<std::string> done_reason;
    std::optional<std::string> error;
    std::optional<std::int32_t> prompt_eval_count;
    std::optional<std::int32_t> eval_count;
};

// ---------------------------------------------------------------------------------------------
// The request pipeline — buildApiMessages + buildRequestBody, the options/think assembly, the
// validation gate, serialization and base64 preparation. Every step order and wire shape is
// pinned by the golden traces.
// ---------------------------------------------------------------------------------------------

std::vector<OllamaMessage> build_api_messages(const openai::ProviderConfig& config,
                                              const std::vector<prep::ChatMessage>& messages,
                                              const std::optional<std::string>& system_prompt,
                                              openai::Base64FileRegistry& base64_files);

struct BuiltOllamaRequest {
    std::vector<OllamaMessage> api_messages;
    std::string body_json;
    std::optional<openai::StreamingJsonBody> streaming_body;
};

BuiltOllamaRequest build_ollama_request(const openai::ProviderConfig& config,
                                        const std::vector<prep::ChatMessage>& messages,
                                        const std::optional<std::string>& system_prompt);

/** OllamaChatRequest.requireValidWireFormat — the provider string is the hardcoded "Ollama". */
void require_valid_wire_format(const OllamaChatRequest& request);

std::string write_ollama_message(const OllamaMessage& message);
std::string write_ollama_message_list(const std::vector<OllamaMessage>& messages);

/** `Json { ignoreUnknownKeys = true }.decodeFromJsonElement(...)` for the validate harness. */
OllamaChatRequest decode_ollama_chat_request(const json::Value& value);

// ---------------------------------------------------------------------------------------------
// The stream loop — the NDJSON half of generateResponse: response decode, the batched tool-call
// snapshot handling, the termination input (doneReason trimmed/lowercased) and usage.
// ---------------------------------------------------------------------------------------------

std::optional<OllamaStreamResponse> decode_ollama_stream_response(std::string_view json_text,
                                                                  DecodeFailure* failure);

/** OllamaStreamResponse.toTokenUsage (coercions, no cache split, no reasoning). */
TokenUsage to_token_usage(const OllamaStreamResponse& response);

/** ollamaStreamTermination: doneReason trim + non-empty + lowercase feeds stopReason. */
sn::StreamTermination ollama_stream_termination(bool saw_done,
                                                const std::optional<std::string>& done_reason,
                                                bool produced_content, bool tool_call_in_flight,
                                                const std::optional<sn::GenerationError>& stream_error,
                                                bool timed_out);

struct UsageUpdate {
    TokenUsage usage;
};

using StreamEvent = std::variant<sn::TextChunk, sn::ThoughtChunk, sn::ToolCallUpdate,
                                 sn::ToolCallRequest, sn::ToolCallsRequest, UsageUpdate>;

/** StreamTermination.carriesModelOutput for these variants. */
bool carries_model_output(const StreamEvent& event);

class OllamaRouter {
public:
    /** [uuid] supplies stream keys and call-id fallbacks (dashed v4 in production). */
    explicit OllamaRouter(sn::UuidFn uuid);

    std::vector<StreamEvent> route(const OllamaStreamResponse& response);

    /** The loop's decode-failure path: SseParse over the raw line (UTF-16 take(512)). */
    void capture_parse_error(std::string_view raw_line, std::string cause);

    // -- terminal state, mirroring the provider loop's locals --------------------------------
    bool saw_done = false;
    std::optional<std::string> done_reason;
    std::optional<sn::GenerationError> stream_error;

private:
    sn::UuidFn uuid_;
};

}  // namespace agora::core::ollama
