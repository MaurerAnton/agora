#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agora/core/citations.hpp"
#include "agora/core/gemini_request.hpp"
#include "agora/core/json.hpp"
#include "agora/core/stream_normalization.hpp"

namespace agora::core::gemini_stream {

namespace sn = agora::core::stream_normalization;
namespace gemini = agora::core::gemini;

// ---------------------------------------------------------------------------------------------
// Decode failure carrier for the driver: the kotlinx cause text is not pinned (only the raw
// line reaches the golden traces), so a descriptive message is enough.
// ---------------------------------------------------------------------------------------------

struct DecodeFailure {
    std::string message;
};

// ---------------------------------------------------------------------------------------------
// TokenUsage — model/TokenUsage.kt, the shape ApiUsageMetadata.toTokenUsage() produces.
// ---------------------------------------------------------------------------------------------

struct TokenUsage {
    std::int32_t total_token_count = 0;
    std::optional<std::int32_t> input_token_count;
    std::optional<std::int32_t> cached_input_token_count;
    std::optional<std::int32_t> uncached_input_token_count;
    std::optional<std::int32_t> output_token_count;
    std::optional<std::int32_t> reasoning_token_count;
};

// ---------------------------------------------------------------------------------------------
// Response-side wire models — GeminiWireModels.kt (the response half of the stand compiles the
// real declarations; the request half lives in agora::core::gemini and is reused where the same
// Kotlin classes appear on both sides).
// ---------------------------------------------------------------------------------------------

struct ApiError {
    std::optional<std::int32_t> code;
    std::optional<std::string> message;
    std::optional<std::string> status;
};

struct ApiUsageMetadata {
    std::optional<std::int32_t> prompt_token_count;
    std::optional<std::int32_t> cached_content_token_count;
    std::optional<std::int32_t> candidates_token_count;
    std::optional<std::int32_t> total_token_count;
    std::optional<std::int32_t> thoughts_token_count;
};

struct ApiResponsePart {
    std::optional<std::string> text;
    std::optional<json::Value> thought;          // JsonElement? — any primitive or value
    std::optional<std::string> thought_signature;
    std::optional<std::string> reasoning_content;
    std::optional<gemini::ApiExecutableCode> executable_code;
    std::optional<gemini::ApiCodeExecutionResult> code_execution_result;
    std::optional<gemini::GeminiFunctionCall> function_call;
};

struct ApiResponseContent {
    std::optional<std::string> role;
    std::vector<ApiResponsePart> parts;
};

struct ApiCandidate {
    std::optional<ApiResponseContent> content;
    std::optional<std::string> finish_reason;
    std::optional<json::Value> grounding_metadata;
};

struct ApiStreamResponse {
    std::optional<std::vector<ApiCandidate>> candidates;
    std::optional<ApiUsageMetadata> usage_metadata;
    std::optional<ApiError> error;
    std::optional<std::string> outcome;
};

// ---------------------------------------------------------------------------------------------
// Router events. The shapes shared with the stream normalizer are reused directly; the payload
// variants (citations with their record, the full usage) are module-local, like the anthropic
// stream module's.
// ---------------------------------------------------------------------------------------------

struct CitationUpdate {
    citations::CitationRecord citation;
};

struct UsageUpdate {
    TokenUsage usage;
};

using StreamEvent = std::variant<sn::TextChunk, sn::ThoughtChunk, sn::ToolCallUpdate,
                                 sn::ToolCallRequest, sn::HostedToolCallUpdate, CitationUpdate,
                                 UsageUpdate>;

/** StreamTermination.carriesModelOutput for these variants. */
bool carries_model_output(const StreamEvent& event);

// ---------------------------------------------------------------------------------------------
// The stream-side helpers extracted verbatim from GeminiProvider.kt.
// ---------------------------------------------------------------------------------------------

/** extractThoughtTitle: the first `**bold**` run, else the first markdown heading line. */
std::optional<std::string> extract_thought_title(std::string_view content);

/** normalizeGeminiFinishReason: trim + lowercase + '-'→'_' with the max_tokens remap. */
std::string normalize_gemini_finish_reason(std::string_view raw);

sn::HostedToolCallUpdate to_google_search_hosted_update(const json::Value& metadata,
                                                        std::string_view stream_key);

sn::HostedToolCallUpdate to_code_execution_start(const gemini::ApiExecutableCode& code,
                                                 std::string stream_key);

/** isError when the outcome is none of OUTCOME_OK / OK / SUCCESS (after uppercase). */
sn::HostedToolCallUpdate to_code_execution_completion(const gemini::ApiCodeExecutionResult& result,
                                                     std::string stream_key,
                                                     std::string arguments);

/** ApiUsageMetadata.toTokenUsage (coercions, derived output, derived total). */
TokenUsage to_token_usage(const ApiUsageMetadata& metadata);

// ---------------------------------------------------------------------------------------------
// Response decode — Json { ignoreUnknownKeys = true }.decodeFromJsonElement with the @JsonNames
// alternate keys (`executable_code`, `code_execution_result`, `grounding_metadata`).
// ---------------------------------------------------------------------------------------------

std::optional<ApiStreamResponse> decode_api_stream_response(std::string_view json_text,
                                                            DecodeFailure* failure);

// ---------------------------------------------------------------------------------------------
// The router: one route() call per SSE line, mirroring the provider's 200-branch loop.
// ---------------------------------------------------------------------------------------------

class GeminiStreamRouter {
public:
    /** [provider_name] is the provider display name used in the in-band error fallback text;
     *  [uuid] supplies stream keys (dashed v4 in production, deterministic in the tests). */
    GeminiStreamRouter(sn::UuidFn uuid, std::string provider_name);

    std::vector<StreamEvent> route(const ApiStreamResponse& response);

    /** The loop's decode-failure path: SseParse over the raw line (UTF-16 take(512)). */
    void capture_parse_error(std::string_view raw_line, std::string cause);

    // -- terminal state, mirroring the provider loop's locals --------------------------------
    bool saw_done = false;
    std::optional<std::string> finish_reason;
    std::optional<sn::GenerationError> stream_error;

    /** The pending code-execution queue drives the termination's toolCallInFlight input. */
    bool tool_call_in_flight() const;

    const std::string& answer_text() const;
    /** The final citation pass: latest grounding metadata over the full answer text. */
    std::vector<citations::CitationRecord> final_citations() const;

private:
    sn::UuidFn uuid_;
    std::string provider_name_;
    std::string google_search_stream_key_;

    std::optional<std::string> current_thought_signature_;
    bool in_thought_block_ = false;
    std::vector<std::string> completed_tool_call_ids_;
    std::deque<std::pair<std::string, std::string>> pending_code_executions_;
    std::optional<std::string> last_grounding_metadata_;
    std::optional<json::Value> latest_grounding_metadata_;
    std::string answer_text_;
};

}  // namespace agora::core::gemini_stream
