#include "agora/core/gemini_stream.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/stream_normalization.hpp"

namespace agora::core::gemini_stream {
namespace {

namespace json = agora::core::json;
namespace sn = agora::core::stream_normalization;
namespace gemini = agora::core::gemini;
namespace citations = agora::core::citations;

using sn::GenerationError;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

bool wire_safe(std::string_view value, std::size_t limit) {
    if (value.empty() || value.size() > limit) return false;
    for (char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string upper_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

/** Int.coerceAtLeast(0). */
std::optional<std::int32_t> coerce_zero(const std::optional<std::int32_t>& value) {
    if (!value) return std::nullopt;
    return std::max(*value, static_cast<std::int32_t>(0));
}

/** TokenUsage.addCounts: saturating Int addition. */
std::int32_t add_counts(std::int32_t left, std::int32_t right) {
    const std::int64_t sum = static_cast<std::int64_t>(left) + static_cast<std::int64_t>(right);
    if (sum > std::numeric_limits<std::int32_t>::max()) return std::numeric_limits<std::int32_t>::max();
    return static_cast<std::int32_t>(sum);
}

/** JsonObject.stringContent: a non-blank primitive's content. */
std::optional<std::string> primitive_string_content(const json::Value& object,
                                                    std::string_view key) {
    const json::Value* value = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == key) value = &entry.value;  // last duplicate wins
    }
    if (value == nullptr) return std::nullopt;
    if (value->kind != json::Kind::String && value->kind != json::Kind::Literal) {
        return std::nullopt;
    }
    if (json::is_blank(value->text)) return std::nullopt;
    return value->text;
}

const json::Value* member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

/** @JsonNames lookup: the last member whose key is any of the declared names wins. */
const json::Value* member_any(const json::Value& object,
                              std::initializer_list<std::string_view> names) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        for (std::string_view name : names) {
            if (entry.key == name) {
                found = &entry.value;
                break;
            }
        }
    }
    return found;
}

// ---------------------------------------------------------------------------------------------
// Response decode helpers (the number grammar mirrors the request modules' pinned one).
// ---------------------------------------------------------------------------------------------

[[noreturn]] void throw_decode(const std::string& what) {
    throw DecodeFailure{what};
}

std::string decode_string(const json::Value& value) {
    if (value.kind != json::Kind::String) throw_decode("expected a JSON string");
    return value.text;
}

std::optional<std::string> decode_optional_string(const json::Value& object,
                                                  std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_string(*value);
}

bool is_json_number_lexeme(std::string_view text) {
    std::size_t index = 0;
    if (index < text.size() && text[index] == '-') ++index;
    std::size_t digits = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        ++index;
        ++digits;
    }
    if (digits == 0) return false;
    if (index < text.size() && text[index] == '.') {
        ++index;
        digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
            ++digits;
        }
        if (digits == 0) return false;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) ++index;
        digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
            ++digits;
        }
        if (digits == 0) return false;
    }
    return index == text.size();
}

std::int32_t decode_int_value(const json::Value& value) {
    if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) {
        throw_decode("expected an integer");
    }
    const std::string_view text = value.text;
    if (!is_json_number_lexeme(text)) throw_decode("expected an Int");
    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '-') {
        negative = true;
        ++index;
    }
    std::uint64_t accumulator = 0;
    const std::uint64_t limit = negative ? 2147483648ull : 2147483647ull;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') throw_decode("expected an Int");
        accumulator = accumulator * 10 + static_cast<std::uint64_t>(c - '0');
        if (accumulator > limit) throw_decode("expected an Int");
    }
    if (negative) {
        if (accumulator == 2147483648ull) return INT32_MIN;
        return -static_cast<std::int32_t>(accumulator);
    }
    return static_cast<std::int32_t>(accumulator);
}

std::optional<std::int32_t> decode_optional_int(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_int_value(*value);
}

const json::Value& require_object(const json::Value* value, std::string_view what) {
    if (value == nullptr || is_null(value)) throw_decode(std::string(what) + ": unexpected null");
    if (value->kind != json::Kind::Object) throw_decode(std::string(what) + ": expected an object");
    return *value;
}

ApiResponsePart decode_part(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("part must be an object");
    ApiResponsePart part;
    part.text = decode_optional_string(value, "text");
    if (const json::Value* thought = member(value, "thought");
        thought != nullptr && !is_null(thought)) {
        part.thought = *thought;
    }
    part.thought_signature = decode_optional_string(value, "thoughtSignature");
    part.reasoning_content = decode_optional_string(value, "reasoning_content");
    if (const json::Value* code =
            member_any(value, {"executableCode", "executable_code"});
        code != nullptr && !is_null(code)) {
        const json::Value& object = require_object(code, "executableCode");
        gemini::ApiExecutableCode decoded;
        const json::Value* language = member(object, "language");
        if (language == nullptr) throw DecodeFailure{"Field 'language' is required"};
        decoded.language = decode_string(*language);
        const json::Value* body = member(object, "code");
        if (body == nullptr) throw DecodeFailure{"Field 'code' is required"};
        decoded.code = decode_string(*body);
        part.executable_code = std::move(decoded);
    }
    if (const json::Value* result =
            member_any(value, {"codeExecutionResult", "code_execution_result"});
        result != nullptr && !is_null(result)) {
        const json::Value& object = require_object(result, "codeExecutionResult");
        gemini::ApiCodeExecutionResult decoded;
        const json::Value* outcome = member(object, "outcome");
        if (outcome == nullptr) throw DecodeFailure{"Field 'outcome' is required"};
        decoded.outcome = decode_string(*outcome);
        const json::Value* output = member(object, "output");
        if (output == nullptr) throw DecodeFailure{"Field 'output' is required"};
        decoded.output = decode_string(*output);
        part.code_execution_result = std::move(decoded);
    }
    if (const json::Value* call = member(value, "functionCall");
        call != nullptr && !is_null(call)) {
        const json::Value& object = require_object(call, "functionCall");
        gemini::GeminiFunctionCall decoded;
        decoded.id = decode_optional_string(object, "id");
        const json::Value* name = member(object, "name");
        if (name == nullptr) throw DecodeFailure{"Field 'name' is required"};
        decoded.name = decode_string(*name);
        if (const json::Value* args = member(object, "args");
            args != nullptr && !is_null(args)) {
            if (args->kind != json::Kind::Object) throw_decode("args must be an object");
            decoded.args = *args;
        }
        decoded.thought_signature = decode_optional_string(object, "thought_signature");
        part.function_call = std::move(decoded);
    }
    return part;
}

ApiResponseContent decode_content(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("content must be an object");
    ApiResponseContent content;
    content.role = decode_optional_string(value, "role");
    const json::Value* parts = member(value, "parts");
    if (parts == nullptr) throw DecodeFailure{"Field 'parts' is required"};
    if (is_null(parts)) throw_decode("unexpected null");
    if (parts->kind != json::Kind::Array) throw_decode("expected an array");
    for (const json::Value& entry : parts->elements) {
        content.parts.push_back(decode_part(entry));
    }
    return content;
}

ApiCandidate decode_candidate(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("candidate must be an object");
    ApiCandidate candidate;
    if (const json::Value* content = member(value, "content");
        content != nullptr && !is_null(content)) {
        candidate.content = decode_content(*content);
    }
    candidate.finish_reason = decode_optional_string(value, "finishReason");
    if (const json::Value* metadata =
            member_any(value, {"groundingMetadata", "grounding_metadata"});
        metadata != nullptr && !is_null(metadata)) {
        candidate.grounding_metadata = *metadata;
    }
    return candidate;
}

ApiUsageMetadata decode_usage(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("usageMetadata must be an object");
    ApiUsageMetadata usage;
    usage.prompt_token_count = decode_optional_int(value, "promptTokenCount");
    usage.cached_content_token_count = decode_optional_int(value, "cachedContentTokenCount");
    usage.candidates_token_count = decode_optional_int(value, "candidatesTokenCount");
    usage.total_token_count = decode_optional_int(value, "totalTokenCount");
    usage.thoughts_token_count = decode_optional_int(value, "thoughtsTokenCount");
    return usage;
}

ApiError decode_error(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("error must be an object");
    ApiError error;
    error.code = decode_optional_int(value, "code");
    error.message = decode_optional_string(value, "message");
    error.status = decode_optional_string(value, "status");
    return error;
}

// ---------------------------------------------------------------------------------------------
// extractThoughtTitle — THOUGHT_TITLE_BOLD = \*\*(.*?)\*\* first (no DOTALL), then
// THOUGHT_TITLE_HEADING = (?m)^#+\s*(.*)$ (Java \s crosses line breaks).
// ---------------------------------------------------------------------------------------------

constexpr bool is_java_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\f' || c == '\r';
}

}  // namespace

std::optional<std::string> extract_thought_title(std::string_view content) {
    // Bold: leftmost opening `**`, shortest inner run that stays on the same line ('.' excludes
    // '\n'), so the closing `**` must appear before the next line break.
    for (std::size_t i = 0; i + 1 < content.size(); ++i) {
        if (content[i] != '*' || content[i + 1] != '*') continue;
        std::size_t line_end = i + 2;
        while (line_end < content.size() && content[line_end] != '\n') ++line_end;
        for (std::size_t j = i + 2; j + 1 < line_end; ++j) {
            if (content[j] == '*' && content[j + 1] == '*') {
                return std::string(content.substr(i + 2, j - (i + 2)));
            }
        }
    }
    // Heading: first line (string start or after '\n') that begins with '#', then \s* (greedy,
    // crossing line breaks), then the rest of the line.
    std::size_t pos = 0;
    while (pos < content.size()) {
        if (content[pos] == '#') {
            std::size_t i = pos;
            while (i < content.size() && content[i] == '#') ++i;
            while (i < content.size() && is_java_space(content[i])) ++i;
            std::size_t capture_end = i;
            while (capture_end < content.size() && content[capture_end] != '\n') ++capture_end;
            return std::string(content.substr(i, capture_end - i));
        }
        const std::size_t newline = content.find('\n', pos);
        if (newline == std::string_view::npos) break;
        pos = newline + 1;
    }
    return std::nullopt;
}

std::string normalize_gemini_finish_reason(std::string_view raw) {
    std::string value = json::trim(raw);
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '-') c = '_';
    }
    if (value == "max_tokens" || value == "max_output_tokens") return "max_output_tokens";
    return value;
}

sn::HostedToolCallUpdate to_google_search_hosted_update(const json::Value& metadata,
                                                        std::string_view stream_key) {
    // queries = webSearchQueries.mapNotNull { primitive content non-blank }
    std::vector<std::string> queries;
    if (const json::Value* raw_queries = member(metadata, "webSearchQueries");
        raw_queries != nullptr && raw_queries->kind == json::Kind::Array) {
        for (const json::Value& entry : raw_queries->elements) {
            if (entry.kind != json::Kind::String && entry.kind != json::Kind::Literal) continue;
            if (json::is_blank(entry.text)) continue;
            queries.push_back(entry.text);
        }
    }
    // results = groundingChunks.mapNotNull { chunk.web.uri (deduped) with title fallback }
    json::Value results;
    results.kind = json::Kind::Array;
    std::vector<std::string> seen_urls;
    if (const json::Value* chunks = member(metadata, "groundingChunks");
        chunks != nullptr && chunks->kind == json::Kind::Array) {
        for (const json::Value& chunk : chunks->elements) {
            if (chunk.kind != json::Kind::Object) continue;
            const json::Value* web_value = member(chunk, "web");
            if (web_value == nullptr || web_value->kind != json::Kind::Object) continue;
            const json::Value& web = *web_value;
            const std::optional<std::string> url = primitive_string_content(web, "uri");
            if (!url) continue;
            if (std::find(seen_urls.begin(), seen_urls.end(), *url) != seen_urls.end()) continue;
            seen_urls.push_back(*url);
            const std::string title = primitive_string_content(web, "title").value_or(*url);
            json::Value entry;
            entry.kind = json::Kind::Object;
            json::Value title_value;
            title_value.kind = json::Kind::String;
            title_value.text = title;
            entry.members.push_back({"title", title_value});
            json::Value url_value;
            url_value.kind = json::Kind::String;
            url_value.text = *url;
            entry.members.push_back({"url", url_value});
            results.elements.push_back(entry);
        }
    }

    std::string query;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        if (i > 0) query += " · ";
        query += queries[i];
    }

    json::Value arguments;
    arguments.kind = json::Kind::Object;
    if (!json::is_blank(query)) {
        json::Value query_value;
        query_value.kind = json::Kind::String;
        query_value.text = query;
        arguments.members.push_back({"query", query_value});
    }

    json::Value normalized;
    normalized.kind = json::Kind::Object;
    json::Value type_value;
    type_value.kind = json::Kind::String;
    type_value.text = "web_search";
    normalized.members.push_back({"type", type_value});
    json::Value provider_value;
    provider_value.kind = json::Kind::String;
    provider_value.text = "Google";
    normalized.members.push_back({"provider", provider_value});
    if (!json::is_blank(query)) {
        json::Value query_member;
        query_member.kind = json::Kind::String;
        query_member.text = query;
        normalized.members.push_back({"query", query_member});
    }
    normalized.members.push_back({"results", results});
    normalized.members.push_back({"grounding_metadata", metadata});

    sn::HostedToolCallUpdate update;
    update.stream_key = std::string(stream_key);
    update.name = "google_search";
    update.arguments = json::to_kotlin_string(arguments);
    update.result = json::to_kotlin_string(normalized);
    update.is_error = false;
    return update;
}

sn::HostedToolCallUpdate to_code_execution_start(const gemini::ApiExecutableCode& code,
                                                 std::string stream_key) {
    json::Value arguments;
    arguments.kind = json::Kind::Object;
    json::Value language;
    language.kind = json::Kind::String;
    language.text = code.language;
    arguments.members.push_back({"language", language});
    json::Value body;
    body.kind = json::Kind::String;
    body.text = code.code;
    arguments.members.push_back({"code", body});

    sn::HostedToolCallUpdate update;
    update.stream_key = std::move(stream_key);
    update.name = "code_execution";
    update.arguments = json::to_kotlin_string(arguments);
    update.is_error = false;
    return update;
}

sn::HostedToolCallUpdate to_code_execution_completion(const gemini::ApiCodeExecutionResult& result,
                                                     std::string stream_key,
                                                     std::string arguments) {
    json::Value normalized;
    normalized.kind = json::Kind::Object;
    json::Value type_value;
    type_value.kind = json::Kind::String;
    type_value.text = "code_execution";
    normalized.members.push_back({"type", type_value});
    json::Value outcome;
    outcome.kind = json::Kind::String;
    outcome.text = result.outcome;
    normalized.members.push_back({"outcome", outcome});
    json::Value output;
    output.kind = json::Kind::String;
    output.text = result.output;
    normalized.members.push_back({"output", output});

    sn::HostedToolCallUpdate update;
    update.stream_key = std::move(stream_key);
    update.name = "code_execution";
    update.arguments = std::move(arguments);
    update.result = json::to_kotlin_string(normalized);
    const std::string upper = upper_ascii(result.outcome);
    update.is_error = upper != "OUTCOME_OK" && upper != "OK" && upper != "SUCCESS";
    return update;
}

TokenUsage to_token_usage(const ApiUsageMetadata& metadata) {
    const std::optional<std::int32_t> input = coerce_zero(metadata.prompt_token_count);
    const std::optional<std::int32_t> cached = coerce_zero(metadata.cached_content_token_count);
    std::optional<std::int32_t> uncached;
    if (input && cached) uncached = std::max(*input - *cached, static_cast<std::int32_t>(0));
    const std::optional<std::int32_t> visible_output = coerce_zero(metadata.candidates_token_count);
    const std::optional<std::int32_t> reasoning = coerce_zero(metadata.thoughts_token_count);
    std::optional<std::int32_t> output;
    if (visible_output && reasoning) {
        output = add_counts(*visible_output, *reasoning);
    } else if (visible_output) {
        output = *visible_output;
    } else if (reasoning) {
        output = *reasoning;
    }
    std::int32_t derived_total = 0;
    if (input && output) {
        derived_total = add_counts(*input, *output);
    } else if (input) {
        derived_total = *input;
    } else {
        derived_total = output.value_or(0);
    }

    TokenUsage usage;
    usage.total_token_count =
        std::max(metadata.total_token_count.value_or(derived_total), static_cast<std::int32_t>(0));
    usage.input_token_count = input;
    usage.cached_input_token_count = cached;
    usage.uncached_input_token_count = uncached;
    usage.output_token_count = output;
    usage.reasoning_token_count = reasoning;
    return usage;
}

std::optional<ApiStreamResponse> decode_api_stream_response(std::string_view json_text,
                                                            DecodeFailure* failure) {
    try {
        const json::ParseResult parsed = json::parse(json_text);
        if (!parsed.value || parsed.value->kind != json::Kind::Object) {
            throw_decode("expected an object");
        }
        const json::Value& value = *parsed.value;
        ApiStreamResponse response;
        if (const json::Value* candidates = member(value, "candidates");
            candidates != nullptr && !is_null(candidates)) {
            if (candidates->kind != json::Kind::Array) throw_decode("expected an array");
            std::vector<ApiCandidate> decoded;
            for (const json::Value& entry : candidates->elements) {
                decoded.push_back(decode_candidate(entry));
            }
            response.candidates = std::move(decoded);
        }
        if (const json::Value* usage = member(value, "usageMetadata");
            usage != nullptr && !is_null(usage)) {
            response.usage_metadata = decode_usage(*usage);
        }
        if (const json::Value* error = member(value, "error");
            error != nullptr && !is_null(error)) {
            response.error = decode_error(*error);
        }
        response.outcome = decode_optional_string(value, "outcome");
        return response;
    } catch (const DecodeFailure& failure_value) {
        if (failure != nullptr) failure->message = failure_value.message;
        return std::nullopt;
    } catch (const std::exception& error_value) {
        if (failure != nullptr) failure->message = error_value.what();
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------
// Carries-model-output (StreamTermination.kt's rule for these variants).
// ---------------------------------------------------------------------------------------------

bool carries_model_output(const StreamEvent& event) {
    struct Visitor {
        bool operator()(const sn::TextChunk& chunk) const { return !chunk.text.empty(); }
        bool operator()(const sn::ThoughtChunk& chunk) const {
            return !chunk.thought.empty() || chunk.signature.has_value();
        }
        bool operator()(const sn::ToolCallUpdate&) const { return true; }
        bool operator()(const sn::ToolCallRequest&) const { return true; }
        bool operator()(const sn::HostedToolCallUpdate&) const { return true; }
        bool operator()(const CitationUpdate&) const { return false; }
        bool operator()(const UsageUpdate&) const { return false; }
    };
    return std::visit(Visitor{}, event);
}

// ---------------------------------------------------------------------------------------------
// The router.
// ---------------------------------------------------------------------------------------------

GeminiStreamRouter::GeminiStreamRouter(sn::UuidFn uuid, std::string provider_name)
    : uuid_(std::move(uuid)),
      provider_name_(std::move(provider_name)),
      google_search_stream_key_("gemini_google_search_" + uuid_()) {}

bool GeminiStreamRouter::tool_call_in_flight() const {
    return !pending_code_executions_.empty();
}

const std::string& GeminiStreamRouter::answer_text() const { return answer_text_; }

std::vector<citations::CitationRecord> GeminiStreamRouter::final_citations() const {
    if (!latest_grounding_metadata_) return {};
    return citations::to_gemini_citations(*latest_grounding_metadata_, answer_text_);
}

void GeminiStreamRouter::capture_parse_error(std::string_view raw_line, std::string cause) {
    stream_error = sn::ErrorSseParse{json::take_utf16_units(raw_line, 512), std::move(cause)};
}

std::vector<StreamEvent> GeminiStreamRouter::route(const ApiStreamResponse& response) {
    std::vector<StreamEvent> out;

    if (response.error) {
        sn::ErrorApi error;
        if (response.error->code) error.code = std::to_string(*response.error->code);
        error.type = response.error->status;
        const std::string fallback =
            provider_name_ + " reported an error in the response stream";
        if (response.error->message && !json::is_blank(*response.error->message)) {
            error.message = *response.error->message;
        } else {
            error.message = fallback;
        }
        stream_error = error;
    }
    if (!stream_error &&
        sn::ProviderRetryPolicy::is_failed_to_generate_outcome(response.outcome)) {
        stream_error = sn::ErrorApi{std::nullopt, "failed_to_generate",
                                    response.outcome.value_or("")};
    }

    const ApiCandidate* candidate =
        (!response.candidates || response.candidates->empty())
            ? nullptr
            : &response.candidates->front();

    if (candidate && candidate->finish_reason && !json::is_blank(*candidate->finish_reason)) {
        finish_reason = normalize_gemini_finish_reason(*candidate->finish_reason);
    }
    if (candidate && candidate->grounding_metadata) {
        latest_grounding_metadata_ = *candidate->grounding_metadata;
        const std::string snapshot = json::to_kotlin_string(*candidate->grounding_metadata);
        if (!last_grounding_metadata_ || snapshot != *last_grounding_metadata_) {
            last_grounding_metadata_ = snapshot;
            out.push_back(to_google_search_hosted_update(*candidate->grounding_metadata,
                                                         google_search_stream_key_));
        }
    }

    in_thought_block_ = false;
    if (candidate && candidate->content) {
        for (const ApiResponsePart& part : candidate->content->parts) {
            bool is_part_of_thought = false;
            std::optional<std::string> part_thought_signature;
            if (part.thought_signature && !json::is_blank(*part.thought_signature)) {
                current_thought_signature_ = *part.thought_signature;
                part_thought_signature = *part.thought_signature;
                is_part_of_thought = true;
                in_thought_block_ = true;
            }
            if (part.thought) {
                if (part.thought->kind == json::Kind::String) {
                    const std::string& content = part.thought->text;
                    if (!content.empty()) {
                        out.push_back(sn::ThoughtChunk{content, extract_thought_title(content),
                                                       current_thought_signature_});
                        is_part_of_thought = true;
                        in_thought_block_ = true;
                    }
                } else if (part.thought->text == "true") {
                    is_part_of_thought = true;
                    in_thought_block_ = true;
                }
            }
            if (part.reasoning_content && !part.reasoning_content->empty()) {
                out.push_back(sn::ThoughtChunk{*part.reasoning_content,
                                               extract_thought_title(*part.reasoning_content),
                                               current_thought_signature_});
                is_part_of_thought = true;
                in_thought_block_ = true;
            }
            if (part.text && !part.text->empty()) {
                if (is_part_of_thought || in_thought_block_) {
                    out.push_back(sn::ThoughtChunk{*part.text, extract_thought_title(*part.text),
                                                   current_thought_signature_});
                    in_thought_block_ = false;
                } else {
                    answer_text_ += *part.text;
                    out.push_back(sn::TextChunk{*part.text});
                }
            }
            if (part.executable_code) {
                sn::HostedToolCallUpdate active = to_code_execution_start(
                    *part.executable_code, "gemini_code_execution_" + uuid_());
                pending_code_executions_.emplace_back(active.stream_key, active.arguments);
                out.push_back(std::move(active));
            }
            if (part.code_execution_result) {
                std::string stream_key;
                std::string arguments = "{}";
                if (!pending_code_executions_.empty()) {
                    stream_key = pending_code_executions_.front().first;
                    arguments = pending_code_executions_.front().second;
                    pending_code_executions_.pop_front();
                } else {
                    stream_key = "gemini_code_execution_" + uuid_();
                }
                out.push_back(to_code_execution_completion(*part.code_execution_result,
                                                           std::move(stream_key),
                                                           std::move(arguments)));
            }
            if (part.function_call) {
                const gemini::GeminiFunctionCall& call = *part.function_call;
                std::string call_id;
                if (call.id && !json::is_blank(*call.id)) {
                    call_id = *call.id;
                } else {
                    call_id = "call_" + uuid_();
                }
                const std::string args_json =
                    call.args ? json::to_kotlin_string(*call.args) : "{}";
                std::optional<std::string> signature = part_thought_signature;
                if (!signature && call.thought_signature &&
                    !json::is_blank(*call.thought_signature)) {
                    signature = *call.thought_signature;
                }
                if (!signature) signature = current_thought_signature_;
                const std::string stream_key = "call_stream_" + uuid_();
                // The add() only runs when the name and id are wire-safe (Kotlin &&).
                const bool name_safe = wire_safe(call.name, 64);
                const bool id_safe = wire_safe(call_id, 128);
                bool first_time = true;
                for (const std::string& seen : completed_tool_call_ids_) {
                    if (seen == call_id) {
                        first_time = false;
                        break;
                    }
                }
                const bool accepted =
                    name_safe && id_safe && first_time;
                if (accepted) {
                    completed_tool_call_ids_.push_back(call_id);
                    out.push_back(sn::ToolCallUpdate{stream_key, call_id, call.name, args_json,
                                                     signature});
                    out.push_back(sn::ToolCallRequest{call_id, call.name, args_json, signature,
                                                      stream_key, 0});
                } else {
                    out.push_back(sn::malformed_tool_call_request(
                        "Gemini returned invalid or duplicate tool metadata", call.name,
                        args_json, stream_key, signature, uuid_));
                }
                current_thought_signature_.reset();
                in_thought_block_ = false;
            }
        }
    }

    if (candidate && candidate->grounding_metadata) {
        for (const citations::CitationRecord& record :
             citations::to_gemini_citations(*candidate->grounding_metadata, answer_text_)) {
            out.push_back(CitationUpdate{record});
        }
    }
    if (response.usage_metadata) {
        out.push_back(UsageUpdate{to_token_usage(*response.usage_metadata)});
    }
    return out;
}

}  // namespace agora::core::gemini_stream
