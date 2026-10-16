// The transform half of the message-preparation port: protocol validation, Compact projection,
// context trimming, window splits and the API projections. Mirrors the Kotlin sources of
// api/util/{ToolMessages,ApiMessagePreparation,ContextWindow,ContextLimiter}.kt — the golden
// traces under core/tools/golden/message-preparation pin every output byte.
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/message_preparation.hpp"
#include "agora/core/sha256.hpp"

namespace agora::core::prep {
namespace {

namespace json = agora::core::json;

constexpr std::string_view kToolMsgPrefix = "tool_";
constexpr std::string_view kResultMsgPrefix = "result_";
constexpr std::string_view kCompactMsgPrefix = "compact_";
constexpr std::string_view kContextSummaryIdPrefix = "context_summary_";
constexpr std::string_view kApiInitialUserPrefix = "api_initial_user_";
constexpr std::string_view kApiCompactContinuationText = "Please continue.";
constexpr std::string_view kContextSummaryOpenTag = "<context_summary>";
constexpr std::string_view kContextSummaryCloseTag = "</context_summary>";
constexpr std::string_view kImageContextPrefix = "image_context_";
constexpr std::string_view kProtocolNoticePrefix = "protocol_notice_";
constexpr std::string_view kImageTranscriptionBlock =
    "\n\n--- Image Transcription: view_image ---\n";

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

/** SafeWireNames: full-match wire grammars for tool call ids and names. */
bool matches_wire_name(const std::string& value, std::size_t max_units) {
    if (value.empty() || value.size() > max_units) return false;
    for (unsigned char byte : value) {
        const bool safe = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= 'a' && byte <= 'z') || byte == '_' || byte == '-';
        if (!safe) return false;
    }
    return true;
}

bool safe_wire_tool_call_id(const std::string& value) { return matches_wire_name(value, 128); }
bool safe_wire_tool_name(const std::string& value) { return matches_wire_name(value, 64); }

/** "call_" + sha256-hex prefix: the stable digest ids of the projections. */
std::string digest8_hex(std::string_view seed) {
    const std::array<std::uint8_t, 32> digest = agora::core::sha256(seed);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int index = 0; index < 8; ++index) {
        out.push_back(kHex[digest[index] >> 4]);
        out.push_back(kHex[digest[index] & 0x0F]);
    }
    return out;
}

std::string strip_context_summary_prefix(std::string id) {
    if (starts_with(id, kContextSummaryIdPrefix)) {
        id.erase(0, kContextSummaryIdPrefix.size());
    }
    return id;
}

}  // namespace

bool ChatMessage::is_tool_protocol_message() const {
    return starts_with(id, kToolMsgPrefix) || starts_with(id, kResultMsgPrefix);
}

bool ChatMessage::is_context_compact() const { return starts_with(id, kCompactMsgPrefix); }

bool ChatMessage::is_successful_context_compact() const {
    return is_context_compact() && status == MessageStatus::Success;
}

// ---------------------------------------------------------------------------------------------
// ToolMessages.kt — strip / merge / validate
// ---------------------------------------------------------------------------------------------

std::vector<ChatMessage> strip_empty_turns(const std::vector<ChatMessage>& messages) {
    std::vector<ChatMessage> out;
    for (const ChatMessage& message : messages) {
        const bool has_tool_segment = [&] {
            if (!message.segments) return false;
            for (const MessageSegment& segment : *message.segments) {
                if (segment.type == "tool") return true;
            }
            return false;
        }();
        if (!json::is_blank(message.text) || !message.images.empty() ||
            message.is_tool_protocol_message() || message.tool_call || has_tool_segment) {
            out.push_back(message);
        }
    }
    return out;
}

std::vector<ChatMessage> merge_consecutive_same_role(const std::vector<ChatMessage>& messages) {
    if (messages.empty()) return messages;
    std::vector<ChatMessage> result;
    std::size_t index = 0;
    while (index < messages.size()) {
        const ChatMessage& current = messages[index];
        if (current.is_tool_protocol_message()) {
            result.push_back(current);
            ++index;
            continue;
        }
        std::size_t next = index + 1;
        while (next < messages.size()) {
            const ChatMessage& candidate = messages[next];
            if (candidate.is_tool_protocol_message() || candidate.participant != current.participant) {
                break;
            }
            ++next;
        }
        if (next == index + 1) {
            result.push_back(current);
        } else {
            ChatMessage merged = current;
            merged.text.clear();
            merged.images.clear();
            for (std::size_t member = index; member < next; ++member) {
                if (member > index) merged.text += "\n\n";
                merged.text += messages[member].text;
                merged.images.insert(merged.images.end(), messages[member].images.begin(),
                                     messages[member].images.end());
            }
            result.push_back(std::move(merged));
        }
        index = next;
    }
    return result;
}

namespace {

/** normalizeArgumentsOrNull: blank or invalid arguments become "{}"; JSON objects re-serialize. */
std::optional<std::string> normalize_arguments_or_null(const std::optional<std::string>& arguments) {
    const std::string source =
        (arguments && !json::is_blank(*arguments)) ? *arguments : std::string{"{}"};
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return std::nullopt;
    return json::to_kotlin_string(*parsed.value);
}

std::string normalize_arguments(const std::optional<std::string>& arguments) {
    if (const auto normalized = normalize_arguments_or_null(arguments)) return *normalized;
    return arguments && !json::is_blank(*arguments) ? *arguments : std::string{"{}"};
}

std::string non_empty_tool_result(std::string_view result) {
    if (!json::is_blank(result)) return std::string(result);
    return "[Tool returned no textual output]";
}

std::vector<MessageSegment> tool_segments_of(const ChatMessage& message) {
    std::vector<MessageSegment> out;
    if (message.segments) {
        for (const MessageSegment& segment : *message.segments) {
            if (segment.type == "tool") out.push_back(segment);
        }
    }
    return out;
}

struct NormalizedToolCalls {
    ChatMessage message;
    std::vector<MessageSegment> segments;
    std::vector<std::string> call_ids;
};

std::optional<NormalizedToolCalls> normalize_tool_calls(
    const ChatMessage& tool_message, const std::vector<std::string>& already_seen_ids) {
    std::vector<MessageSegment> source_segments = tool_segments_of(tool_message);
    if (source_segments.empty() && tool_message.tool_call) {
        const ToolCallData& call = *tool_message.tool_call;
        source_segments.push_back(MessageSegment{
            "tool", "", call.tool_name, call.arguments, call.result, call.tool_call_id, call.signature,
            std::nullopt, std::nullopt, std::nullopt, {}, std::nullopt});
    }
    if (source_segments.empty()) return std::nullopt;

    std::vector<std::string> explicit_ids;
    for (const MessageSegment& segment : source_segments) {
        if (segment.tool_call_id && !json::is_blank(*segment.tool_call_id)) {
            explicit_ids.push_back(*segment.tool_call_id);
        }
    }
    const auto any_unsafe =
        std::any_of(explicit_ids.begin(), explicit_ids.end(),
                    [](const std::string& id) { return !safe_wire_tool_call_id(id); });
    std::vector<std::string> distinct_ids = explicit_ids;
    std::sort(distinct_ids.begin(), distinct_ids.end());
    const bool duplicates =
        std::adjacent_find(distinct_ids.begin(), distinct_ids.end()) != distinct_ids.end();
    const auto any_seen =
        std::any_of(explicit_ids.begin(), explicit_ids.end(), [&](const std::string& id) {
            return std::find(already_seen_ids.begin(), already_seen_ids.end(), id) !=
                   already_seen_ids.end();
        });
    if (any_unsafe || duplicates || any_seen) return std::nullopt;

    std::vector<std::string> reserved = already_seen_ids;
    std::vector<MessageSegment> normalized;
    normalized.reserve(source_segments.size());
    for (std::size_t index = 0; index < source_segments.size(); ++index) {
        const MessageSegment& segment = source_segments[index];
        if (!segment.tool_name || !safe_wire_tool_name(*segment.tool_name)) return std::nullopt;
        const auto arguments = normalize_arguments_or_null(segment.tool_args);
        if (!arguments) return std::nullopt;
        std::string call_id;
        if (segment.tool_call_id && !json::is_blank(*segment.tool_call_id)) {
            call_id = *segment.tool_call_id;
        } else {
            call_id = openai::build_tool_call_id(
                *segment.tool_name + ":" + tool_message.id + ":" + std::to_string(index), *arguments);
        }
        if (std::find(reserved.begin(), reserved.end(), call_id) != reserved.end()) return std::nullopt;
        reserved.push_back(call_id);
        MessageSegment copy = segment;
        copy.tool_name = *segment.tool_name;
        copy.tool_args = *arguments;
        copy.tool_call_id = call_id;
        copy.tool_result.reset();  // Results belong only to the following result_ row.
        normalized.push_back(std::move(copy));
    }

    ChatMessage message = tool_message;
    message.segments.reset();
    if (normalized.size() == 1 && tool_message.tool_call) {
        ToolCallData call = *tool_message.tool_call;
        call.tool_name = normalized[0].tool_name.value_or("");
        call.arguments = normalized[0].tool_args.value_or("");
        call.tool_call_id = normalized[0].tool_call_id;
        message.tool_call = call;
    }
    // Replace only tool segments; every non-tool segment stays in its original order.
    if (tool_message.segments) {
        std::vector<MessageSegment> rebuilt;
        rebuilt.reserve(tool_message.segments->size());
        std::size_t normalized_index = 0;
        for (const MessageSegment& segment : *tool_message.segments) {
            rebuilt.push_back(segment.type == "tool" ? normalized[normalized_index++] : segment);
        }
        message.segments = std::move(rebuilt);
    } else {
        message.segments = normalized;
    }

    NormalizedToolCalls result;
    result.message = std::move(message);
    result.call_ids.reserve(normalized.size());
    for (const MessageSegment& segment : normalized) {
        result.segments.push_back(segment);
        result.call_ids.push_back(segment.tool_call_id.value_or(""));
    }
    return result;
}

/** ResultRef: one payload position of a result batch, by segment or by the row's legacy call. */
struct ResultRef {
    std::size_t message_index;
    const MessageSegment* segment;
    std::optional<std::string> explicit_id;
};

std::optional<std::vector<ChatMessage>> normalize_tool_results(
    const NormalizedToolCalls& calls, const std::vector<ChatMessage>& result_messages) {
    // One owned copy of the filtered segments per result row; the refs point into it.
    std::vector<std::vector<MessageSegment>> owned(result_messages.size());
    std::vector<ResultRef> refs;
    for (std::size_t index = 0; index < result_messages.size(); ++index) {
        const ChatMessage& message = result_messages[index];
        if (message.segments) {
            for (const MessageSegment& segment : *message.segments) {
                if (segment.type == "tool") owned[index].push_back(segment);
            }
        }
        if (!owned[index].empty()) {
            for (MessageSegment& segment : owned[index]) {
                refs.push_back(ResultRef{
                    index, &segment,
                    (segment.tool_call_id && !json::is_blank(*segment.tool_call_id))
                        ? std::optional<std::string>(*segment.tool_call_id)
                        : std::nullopt});
            }
        } else {
            refs.push_back(ResultRef{
                index, nullptr,
                (message.tool_call && message.tool_call->tool_call_id &&
                 !json::is_blank(*message.tool_call->tool_call_id))
                    ? std::optional<std::string>(*message.tool_call->tool_call_id)
                    : std::nullopt});
        }
    }
    if (refs.size() != calls.segments.size()) return std::nullopt;

    bool has_explicit_result_ids = false;
    for (const ResultRef& ref : refs) {
        if (ref.explicit_id) has_explicit_result_ids = true;
    }
    std::vector<std::string> explicit_ids;
    for (const ResultRef& ref : refs) {
        if (ref.explicit_id) explicit_ids.push_back(*ref.explicit_id);
    }
    std::vector<std::string> distinct_explicit = explicit_ids;
    std::sort(distinct_explicit.begin(), distinct_explicit.end());
    const bool explicit_duplicates =
        std::adjacent_find(distinct_explicit.begin(), distinct_explicit.end()) != distinct_explicit.end();
    bool any_null_with_explicit = false;
    for (const ResultRef& ref : refs) {
        if (has_explicit_result_ids && !ref.explicit_id) any_null_with_explicit = true;
    }
    std::vector<std::string> call_ids = calls.call_ids;
    std::sort(call_ids.begin(), call_ids.end());
    std::vector<std::string> sorted_explicit = explicit_ids;
    std::sort(sorted_explicit.begin(), sorted_explicit.end());
    if (std::any_of(explicit_ids.begin(), explicit_ids.end(),
                    [](const std::string& id) { return !safe_wire_tool_call_id(id); }) ||
        any_null_with_explicit || explicit_duplicates ||
        (has_explicit_result_ids && sorted_explicit != call_ids)) {
        return std::nullopt;
    }

    std::vector<ChatMessage> normalized;
    std::size_t call_index = 0;
    const auto call_by_id = [&](const std::string& id) -> const MessageSegment* {
        for (const MessageSegment& segment : calls.segments) {
            if (segment.tool_call_id && *segment.tool_call_id == id) return &segment;
        }
        return nullptr;
    };
    for (std::size_t index = 0; index < result_messages.size(); ++index) {
        ChatMessage row = result_messages[index];
        if (!owned[index].empty()) {
            std::vector<MessageSegment> kept;
            for (MessageSegment& segment : owned[index]) {
                const MessageSegment* call = nullptr;
                if (has_explicit_result_ids) {
                    if (!segment.tool_call_id) return std::nullopt;
                    call = call_by_id(*segment.tool_call_id);
                    if (!call) return std::nullopt;
                } else {
                    if (call_index >= calls.segments.size()) return std::nullopt;
                    call = &calls.segments[call_index];
                }
                ++call_index;
                segment.tool_name = call->tool_name;
                segment.tool_args = call->tool_args;
                segment.tool_call_id = call->tool_call_id;
                segment.tool_result = non_empty_tool_result(
                    segment.tool_result ? *segment.tool_result : row.text);
                segment.signature = call->signature;
                segment.signature_provider = call->signature_provider;
                kept.push_back(std::move(segment));
            }
            if (row.tool_call && kept.size() == 1) {
                ToolCallData call = *row.tool_call;
                call.tool_call_id = kept[0].tool_call_id;
                row.tool_call = call;
            }
            row.segments = std::move(kept);
        } else {
            const ToolCallData* legacy = row.tool_call ? &*row.tool_call : nullptr;
            const MessageSegment* call = nullptr;
            if (has_explicit_result_ids) {
                if (!legacy || !legacy->tool_call_id) return std::nullopt;
                call = call_by_id(*legacy->tool_call_id);
                if (!call) return std::nullopt;
            } else {
                if (call_index >= calls.segments.size()) return std::nullopt;
                call = &calls.segments[call_index];
            }
            ++call_index;
            const std::string result =
                non_empty_tool_result(legacy ? legacy->result : row.text);
            MessageSegment segment;
            segment.type = "tool";
            segment.tool_name = call->tool_name;
            segment.tool_args = call->tool_args;
            segment.tool_call_id = call->tool_call_id;
            segment.tool_result = result;
            segment.signature = call->signature;
            segment.signature_provider = call->signature_provider;
            row.segments = std::vector<MessageSegment>{std::move(segment)};
            if (legacy) {
                ToolCallData copy = *legacy;
                copy.tool_name = call->tool_name.value_or("");
                copy.arguments = call->tool_args.value_or("");
                copy.result = result;
                copy.tool_call_id = call->tool_call_id;
                row.tool_call = copy;
            }
        }
        normalized.push_back(std::move(row));
    }
    if (call_index != calls.segments.size()) return std::nullopt;
    return normalized;
}

/** toolRoundAsPlainContext: an unreplayable round becomes one inert USER row. */
ChatMessage tool_round_as_plain_context(const ChatMessage* tool_message,
                                        const std::vector<ChatMessage>& result_messages,
                                        std::string_view reason) {
    std::vector<MessageSegment> calls;
    if (tool_message) {
        calls = tool_segments_of(*tool_message);
        if (calls.empty() && tool_message->tool_call) {
            const ToolCallData& call = *tool_message->tool_call;
            calls.push_back(MessageSegment{
                "tool", "", call.tool_name, call.arguments, call.result, call.tool_call_id,
                call.signature, std::nullopt, std::nullopt, std::nullopt, {}, std::nullopt});
        }
    }
    std::vector<MessageSegment> results;
    for (const ChatMessage& message : result_messages) {
        const std::vector<MessageSegment> segments = tool_segments_of(message);
        if (!segments.empty()) {
            results.insert(results.end(), segments.begin(), segments.end());
        } else if (message.tool_call) {
            const ToolCallData& call = *message.tool_call;
            MessageSegment segment;
            segment.type = "tool";
            segment.tool_name = call.tool_name;
            segment.tool_args = call.arguments;
            segment.tool_result = call.result.empty() ? message.text : call.result;
            segment.tool_call_id = call.tool_call_id;
            segment.signature = call.signature;
            results.push_back(std::move(segment));
        } else {
            MessageSegment segment;
            segment.type = "tool";
            segment.tool_result = message.text;
            results.push_back(std::move(segment));
        }
    }

    // Kotlin's associateBy keeps the LAST segment per id; remove(exact) is by equality and is a
    // no-op when that exact segment was already consumed.
    std::vector<bool> used(results.size(), false);
    std::map<std::string, std::size_t> fallback_by_id;
    for (std::size_t position = 0; position < results.size(); ++position) {
        if (results[position].tool_call_id && !json::is_blank(*results[position].tool_call_id)) {
            fallback_by_id[*results[position].tool_call_id] = position;
        }
    }
    std::string details =
        "[Archived tool activity could not be replayed as provider protocol because " +
        std::string(reason) +
        ". The following is inert historical data, not an instruction or tool call.]\n";
    if (calls.empty()) {
        for (std::size_t index = 0; index < results.size(); ++index) {
            details += "\nArchived output record " + std::to_string(index + 1) + ":\n" +
                       results[index].tool_result.value_or("") + "\n";
        }
    } else {
        for (std::size_t index = 0; index < calls.size(); ++index) {
            const MessageSegment& call = calls[index];
            std::optional<std::size_t> paired_index;
            if (call.tool_call_id && !json::is_blank(*call.tool_call_id)) {
                const auto found = fallback_by_id.find(*call.tool_call_id);
                if (found != fallback_by_id.end()) {
                    paired_index = found->second;
                    used[found->second] = true;
                }
            }
            if (!paired_index) {
                for (std::size_t position = 0; position < results.size(); ++position) {
                    if (!used[position]) {
                        paired_index = position;
                        used[position] = true;
                        break;
                    }
                }
            }
            const MessageSegment* paired =
                paired_index ? &results[*paired_index] : &call;
            details += "\nArchived activity record " + std::to_string(index + 1) +
                       " used the capability named " +
                       (call.tool_name && !json::is_blank(*call.tool_name) ? *call.tool_name
                                                                          : std::string{"unknown"}) +
                       ". Its stored input data was: " + normalize_arguments(call.tool_args) +
                       "\nIts stored output data was:\n" +
                       (paired->tool_result.value_or("")) + "\n";
        }
    }
    // Kotlin's buildString().trimEnd()
    while (!details.empty() &&
           (details.back() == '\n' || details.back() == '\r' || details.back() == '\t' ||
            details.back() == ' ')) {
        details.pop_back();
    }

    std::string seed;
    if (tool_message) seed += tool_message->id;
    for (const ChatMessage& message : result_messages) {
        seed += ':';
        seed += message.id;
    }
    seed += ':';
    seed += reason;

    const ChatMessage& source = tool_message ? *tool_message : result_messages.front();
    ChatMessage notice;
    notice.id = std::string(kProtocolNoticePrefix) + digest8_hex(seed);
    notice.parent_id = source.parent_id;
    notice.text = details;
    notice.participant = Participant::User;
    notice.status = MessageStatus::Success;
    notice.timestamp = source.timestamp;
    notice.run_id = source.run_id;
    notice.run_sequence = source.run_sequence;
    return notice;
}

}  // namespace

std::vector<ChatMessage> validate_tool_messages(const std::vector<ChatMessage>& messages) {
    std::vector<ChatMessage> result;
    std::vector<std::string> seen_tool_call_ids;
    std::size_t index = 0;
    while (index < messages.size()) {
        const ChatMessage& message = messages[index];
        if (starts_with(message.id, kToolMsgPrefix)) {
            std::vector<ChatMessage> result_messages;
            std::size_t next = index + 1;
            while (next < messages.size() &&
                   starts_with(messages[next].id, kResultMsgPrefix)) {
                result_messages.push_back(messages[next]);
                ++next;
            }
            const auto normalized_calls = normalize_tool_calls(message, seen_tool_call_ids);
            std::optional<std::vector<ChatMessage>> normalized_results;
            if (normalized_calls) normalized_results = normalize_tool_results(*normalized_calls, result_messages);
            if (normalized_calls && normalized_results) {
                result.push_back(normalized_calls->message);
                result.insert(result.end(), normalized_results->begin(), normalized_results->end());
                seen_tool_call_ids.insert(seen_tool_call_ids.end(), normalized_calls->call_ids.begin(),
                                          normalized_calls->call_ids.end());
            } else {
                result.push_back(tool_round_as_plain_context(
                    &message, result_messages,
                    "the stored tool round was incomplete or damaged"));
            }
            index = next;
        } else if (starts_with(message.id, kResultMsgPrefix)) {
            result.push_back(tool_round_as_plain_context(
                nullptr, std::vector<ChatMessage>{message},
                "the stored tool result had no matching tool call"));
            ++index;
        } else {
            result.push_back(message);
            ++index;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// ApiMessagePreparation.kt — Compact projection, canonicalization, prepareMessages
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<ChatMessage> project_nearest_context_compact(const std::vector<ChatMessage>& messages,
                                                        bool mark_summary_for_api,
                                                        bool append_continuation_for_api) {
    // A failed/stopped Compact is durable history with no provider-context meaning.
    const bool has_non_successful_compact = std::any_of(
        messages.begin(), messages.end(), [](const ChatMessage& message) {
            return message.is_context_compact() && !message.is_successful_context_compact();
        });
    std::vector<ChatMessage> provider_visible;
    for (const ChatMessage& message : messages) {
        if (has_non_successful_compact && message.is_context_compact() &&
            !message.is_successful_context_compact()) {
            continue;
        }
        provider_visible.push_back(message);
    }
    std::size_t index = provider_visible.size();
    for (std::size_t position = provider_visible.size(); position > 0; --position) {
        if (provider_visible[position - 1].is_successful_context_compact()) {
            index = position - 1;
            break;
        }
    }
    if (index == provider_visible.size()) return provider_visible;

    const ChatMessage& compact = provider_visible[index];
    std::string projected_text;
    if (mark_summary_for_api) {
        projected_text += kContextSummaryOpenTag;
        projected_text += '\n';
        projected_text += json::trim(compact.text);
        projected_text += '\n';
        projected_text += kContextSummaryCloseTag;
        if (append_continuation_for_api) {
            projected_text += "\n\n";
            projected_text += kApiCompactContinuationText;
        }
    } else {
        projected_text = compact.text;
    }
    std::vector<ChatMessage> out;
    out.reserve(provider_visible.size() - index);
    ChatMessage summary = compact;
    summary.id = std::string(kContextSummaryIdPrefix) + compact.id;
    summary.text = projected_text;
    summary.participant = Participant::User;
    out.push_back(std::move(summary));
    for (std::size_t rest = index + 1; rest < provider_visible.size(); ++rest) {
        out.push_back(provider_visible[rest]);
    }
    return out;
}

std::vector<ChatMessage> canonical_context_messages_with(const std::vector<ChatMessage>& messages,
                                                        bool mark_summary_for_api,
                                                        bool append_continuation_for_api) {
    const std::vector<ChatMessage> compacted =
        project_nearest_context_compact(messages, mark_summary_for_api, append_continuation_for_api);
    std::vector<ChatMessage> distinct;
    std::vector<std::string> seen_ids;
    for (const ChatMessage& message : compacted) {
        if (std::find(seen_ids.begin(), seen_ids.end(), message.id) != seen_ids.end()) continue;
        seen_ids.push_back(message.id);
        distinct.push_back(message);
    }
    const std::vector<ChatMessage> canonical =
        validate_tool_messages(strip_empty_turns(distinct));
    return strip_empty_turns(merge_consecutive_same_role(canonical));
}

bool should_append_compact_continuation(const std::vector<ChatMessage>& messages) {
    std::size_t compact_index = messages.size();
    for (std::size_t position = messages.size(); position > 0; --position) {
        if (messages[position - 1].is_successful_context_compact()) {
            compact_index = position - 1;
            break;
        }
    }
    if (compact_index == messages.size()) return false;
    if (compact_index + 1 >= messages.size()) return true;
    const ChatMessage& adjacent = messages[compact_index + 1];
    return adjacent.participant != Participant::User || adjacent.is_tool_protocol_message() ||
           adjacent.is_context_compact();
}

}  // namespace

std::vector<ChatMessage> apply_nearest_context_compact(const std::vector<ChatMessage>& messages) {
    return project_nearest_context_compact(messages, false, false);
}

std::vector<ChatMessage> canonical_context_messages(const std::vector<ChatMessage>& messages) {
    return canonical_context_messages_with(messages, false, false);
}

std::vector<ChatMessage> prepare_messages(const std::vector<ChatMessage>& messages,
                                          int context_token_budget,
                                          bool include_assistant_reasoning,
                                          const ContextCostModel& costs) {
    const ChatMessage* previous =
        messages.size() >= 2 ? &messages[messages.size() - 2] : nullptr;
    std::optional<ChatMessage> prompt;
    if (!messages.empty()) {
        const ChatMessage& last = messages.back();
        const std::string expected_id =
            std::string(kApiInitialUserPrefix) + (previous ? previous->id : std::string{});
        const std::optional<std::string> previous_id =
            previous ? std::optional<std::string>(previous->id) : std::nullopt;
        if (last.id == expected_id && last.parent_id == previous_id &&
            last.participant == Participant::User) {
            prompt = last;
        }
    }
    std::vector<ChatMessage> history = messages;
    if (prompt) history.pop_back();
    const bool append_continuation = should_append_compact_continuation(messages);
    std::vector<ChatMessage> prepared = strip_empty_turns(merge_consecutive_same_role(limit_context(
        canonical_context_messages_with(history, true, append_continuation),
        context_token_budget, include_assistant_reasoning, costs)));
    if (prompt) prepared.push_back(*prompt);
    return prepared;
}

// ---------------------------------------------------------------------------------------------
// ContextWindow.kt — atomic units, splits, usage, retained ids
// ---------------------------------------------------------------------------------------------

std::vector<std::vector<ChatMessage>> protocol_atomic_units(const std::vector<ChatMessage>& messages) {
    std::vector<std::vector<ChatMessage>> units;
    std::size_t index = 0;
    while (index < messages.size()) {
        const ChatMessage& message = messages[index];
        if (starts_with(message.id, kToolMsgPrefix)) {
            std::vector<ChatMessage> round{message};
            ++index;
            while (index < messages.size() && starts_with(messages[index].id, kResultMsgPrefix)) {
                round.push_back(messages[index]);
                ++index;
            }
            units.push_back(std::move(round));
        } else {
            units.push_back(std::vector<ChatMessage>{message});
            ++index;
        }
    }
    return units;
}

ContextRetentionSplit split_context_for_compact_retention(const std::vector<ChatMessage>& messages,
                                                          int retain_messages) {
    if (retain_messages < 0) {
        throw PrepError{"IllegalArgumentException", "retainMessages must be non-negative"};
    }
    if (messages.empty()) return ContextRetentionSplit{};
    if (retain_messages == 0) {
        return ContextRetentionSplit{messages, {}, 0};
    }
    const std::vector<std::vector<ChatMessage>> units = protocol_atomic_units(messages);
    std::deque<std::vector<ChatMessage>> retained_units;
    int retained_count = 0;
    for (auto iterator = units.rbegin(); iterator != units.rend(); ++iterator) {
        if (retained_count >= retain_messages) break;
        retained_units.push_front(*iterator);
        retained_count += static_cast<int>(iterator->size());
    }
    std::vector<ChatMessage> retained;
    for (const auto& unit : retained_units) {
        retained.insert(retained.end(), unit.begin(), unit.end());
    }
    std::vector<ChatMessage> prefix;
    if (messages.size() > retained.size()) {
        prefix.assign(messages.begin(), messages.end() - static_cast<std::ptrdiff_t>(retained.size()));
    }
    return ContextRetentionSplit{std::move(prefix), std::move(retained), retained_count};
}

LogicalContextSplit split_logical_context(const std::vector<ChatMessage>& messages,
                                          int retain_logical_messages) {
    if (retain_logical_messages < 0) {
        throw PrepError{"IllegalArgumentException", "retainLogicalMessages must be non-negative"};
    }
    if (messages.empty()) return LogicalContextSplit{};
    std::vector<std::pair<std::size_t, Participant>> normal;
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const ChatMessage& message = messages[index];
        if (message.is_tool_protocol_message() || starts_with(message.id, kCompactMsgPrefix)) {
            continue;
        }
        normal.emplace_back(index, message.participant);
    }
    std::vector<std::vector<std::size_t>> groups;
    std::optional<Participant> previous;
    for (const auto& [index, participant] : normal) {
        if (groups.empty() || participant != previous) groups.emplace_back();
        groups.back().push_back(index);
        previous = participant;
    }
    const int count = static_cast<int>(groups.size());
    if (retain_logical_messages <= 0) {
        return LogicalContextSplit{messages, {}, count};
    }
    if (retain_logical_messages >= count) {
        return LogicalContextSplit{{}, messages, count};
    }
    std::ptrdiff_t cut = static_cast<std::ptrdiff_t>(groups[count - retain_logical_messages].front());
    // The cut may land inside a tool round: start at the row before it and pull the round in.
    std::ptrdiff_t cursor = cut - 1;
    while (cursor >= 0 && starts_with(messages[cursor].id, kResultMsgPrefix)) --cursor;
    if (cursor >= 0 && starts_with(messages[cursor].id, kToolMsgPrefix)) cut = cursor;
    return LogicalContextSplit{
        {messages.begin(), messages.begin() + cut},
        {messages.begin() + cut, messages.end()},
        count,
    };
}

float ContextWindowUsage::progress() const {
    if (token_budget <= 0) return 0.0f;
    const float ratio = static_cast<float>(estimated_token_count) /
                        static_cast<float>(token_budget);
    return std::clamp(ratio, 0.0f, 1.0f);
}

std::int32_t ContextWindowUsage::message_tokens() const {
    return std::max(estimated_token_count - system_prompt_tokens - tool_tokens, 0);
}

ContextWindowUsage context_window_usage(const std::vector<ChatMessage>& messages,
                                        int token_budget, int fixed_token_cost,
                                        bool include_assistant_reasoning,
                                        const std::optional<FixedContextComposition>& fixed_composition,
                                        const ContextCostModel& costs) {
    const int safe_budget = std::max(token_budget, 1);
    const std::vector<ChatMessage> canonical = canonical_context_messages(messages);
    const std::int64_t estimated = static_cast<std::int64_t>(
        CostModelContextEstimator(costs).estimate(canonical, include_assistant_reasoning));
    const std::int64_t with_fixed =
        estimated + std::max(fixed_token_cost, 0);
    ContextWindowUsage usage;
    usage.estimated_token_count =
        static_cast<std::int32_t>(std::min(with_fixed, static_cast<std::int64_t>(
                                                            std::numeric_limits<std::int32_t>::max())));
    usage.token_budget = safe_budget;
    usage.logical_message_count = split_logical_context(canonical, 0).logical_message_count;
    usage.has_compact_boundary =
        std::any_of(messages.begin(), messages.end(),
                    [](const ChatMessage& message) { return message.is_successful_context_compact(); });
    usage.system_prompt_tokens = fixed_composition ? fixed_composition->system_prompt_tokens : 0;
    usage.tool_tokens = fixed_composition ? fixed_composition->tool_tokens : 0;
    return usage;
}

std::vector<std::string> context_window_retained_message_ids(
    const std::vector<ChatMessage>& messages, int token_budget, int fixed_token_cost,
    bool include_assistant_reasoning, const ContextCostModel& costs) {
    if (messages.empty()) return {};
    const std::vector<ChatMessage> compacted = apply_nearest_context_compact(messages);
    const int message_budget =
        std::max(token_budget - std::max(fixed_token_cost, 0), 1);
    const std::vector<ChatMessage> retained = limit_context(
        canonical_context_messages(messages), message_budget, include_assistant_reasoning, costs);
    if (retained.empty()) return {};
    const std::string first_retained_id = retained.front().id;
    const std::string source_anchor_id = strip_context_summary_prefix(first_retained_id);

    std::size_t original_source_index = messages.size();
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (messages[index].id == source_anchor_id) {
            original_source_index = index;
            break;
        }
    }
    if (original_source_index < messages.size()) {
        // Recover the durable boundary in the original graph; keep every row from the anchor on.
        std::vector<std::string> ids;
        for (std::size_t index = original_source_index; index < messages.size(); ++index) {
            const ChatMessage& message = messages[index];
            if (message.is_context_compact() && !message.is_successful_context_compact()) continue;
            ids.push_back(message.id);
        }
        return ids;
    }
    std::size_t source_index = compacted.size();
    for (std::size_t index = 0; index < compacted.size(); ++index) {
        if (compacted[index].id == source_anchor_id) {
            source_index = index;
            break;
        }
    }
    if (source_index >= compacted.size()) {
        std::vector<std::string> ids;
        for (const ChatMessage& message : retained) {
            ids.push_back(strip_context_summary_prefix(message.id));
        }
        return ids;
    }
    std::vector<std::string> ids;
    for (std::size_t index = source_index; index < compacted.size(); ++index) {
        ids.push_back(compacted[index].id);
    }
    return ids;
}

// ---------------------------------------------------------------------------------------------
// ContextLimiter.kt — limitContext
// ---------------------------------------------------------------------------------------------

std::vector<ChatMessage> limit_context(const std::vector<ChatMessage>& messages,
                                       int context_token_budget, bool include_assistant_reasoning,
                                       const ContextCostModel& costs) {
    if (messages.empty()) return {};
    const CostModelContextEstimator estimator(costs);
    const std::vector<std::vector<ChatMessage>> units = protocol_atomic_units(messages);

    std::deque<std::vector<ChatMessage>> selected;
    std::int64_t estimated_tokens = 0;
    bool has_normal_user_anchor = false;
    const std::int64_t token_budget = std::max(context_token_budget, 1);
    const auto is_normal_user = [](const ChatMessage& message) {
        return message.participant == Participant::User && !message.is_tool_protocol_message();
    };
    for (auto iterator = units.rbegin(); iterator != units.rend(); ++iterator) {
        const std::vector<ChatMessage>& unit = *iterator;
        const std::int64_t unit_cost =
            estimator.estimate(unit, include_assistant_reasoning);
        if (!selected.empty() && has_normal_user_anchor &&
            estimated_tokens + unit_cost > token_budget) {
            break;
        }
        selected.push_front(unit);
        estimated_tokens = std::min(estimated_tokens + unit_cost,
                                    static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));
        for (const ChatMessage& message : unit) {
            if (is_normal_user(message)) has_normal_user_anchor = true;
        }
        // Always retain a legal user anchor and at least the newest complete unit.
        if (has_normal_user_anchor && estimated_tokens >= token_budget) break;
    }

    std::vector<ChatMessage> flattened;
    for (const auto& unit : selected) {
        flattened.insert(flattened.end(), unit.begin(), unit.end());
    }
    // All supported chat protocols accept a user-led suffix.
    std::size_t first_normal_user = flattened.size();
    for (std::size_t index = 0; index < flattened.size(); ++index) {
        if (is_normal_user(flattened[index])) {
            first_normal_user = index;
            break;
        }
    }
    if (first_normal_user == flattened.size()) return {};
    return {flattened.begin() + static_cast<std::ptrdiff_t>(first_normal_user), flattened.end()};
}

// ---------------------------------------------------------------------------------------------
// ToolMessages.kt — the API projections
// ---------------------------------------------------------------------------------------------

namespace {

bool is_normal_user_message(const ChatMessage& message) {
    return message.participant == Participant::User && !message.is_tool_protocol_message();
}

bool is_normal_assistant_message(const ChatMessage& message) {
    return message.participant == Participant::Model && !message.is_tool_protocol_message();
}

std::string add_generated_image_context_note(const std::string& text, std::size_t image_count) {
    std::string note;
    if (image_count == 1) {
        note = "[Visual context: the first attached image was generated by the assistant earlier "
               "in this conversation.]";
    } else {
        note = "[Visual context: the first " + std::to_string(image_count) +
               " attached images were generated by the assistant earlier in this conversation.]";
    }
    if (json::is_blank(text)) return note;
    return note + "\n\n" + text;
}

std::string transcription_descriptions_for_batch(const std::vector<ChatMessage>& batch) {
    std::vector<std::string> descriptions;
    for (const ChatMessage& message : batch) {
        if (!message.segments) continue;
        for (const MessageSegment& segment : *message.segments) {
            if (!segment.tool_transcription || json::is_blank(*segment.tool_transcription)) {
                continue;
            }
            if (std::find(descriptions.begin(), descriptions.end(), *segment.tool_transcription) !=
                descriptions.end()) {
                continue;
            }
            descriptions.push_back(*segment.tool_transcription);
        }
    }
    std::string joined;
    for (std::size_t index = 0; index < descriptions.size(); ++index) {
        if (index > 0) joined += "\n\n";
        joined += descriptions[index];
    }
    if (json::is_blank(joined)) return "";
    return joined;
}

std::string generation_status_event_text(bool is_error, const std::optional<std::string>& detail) {
    // Machine-shaped rather than a sentence: prose here gets imitated by later answers.
    const char* reason = is_error ? "error" : "stopped";
    const bool has_detail = detail && !json::is_blank(*detail);
    if (!has_detail) {
        return std::string("<generation_interrupted reason=\"") + reason + "\" />";
    }
    return std::string("<generation_interrupted reason=\"") + reason + "\">\n" + *detail +
           "\n</generation_interrupted>";
}

}  // namespace

std::vector<ChatMessage> project_assistant_images_to_latest_user_message(
    const std::vector<ChatMessage>& messages, bool include_images) {
    if (messages.empty()) return messages;

    std::size_t latest_user_index = messages.size();
    for (std::size_t position = messages.size(); position > 0; --position) {
        if (is_normal_user_message(messages[position - 1])) {
            latest_user_index = position - 1;
            break;
        }
    }
    std::vector<std::string> generated_images;
    if (include_images && latest_user_index < messages.size()) {
        std::vector<std::string> last_images;
        for (std::size_t index = 0; index < latest_user_index; ++index) {
            const ChatMessage& message = messages[index];
            if (is_normal_assistant_message(message) && !message.images.empty()) {
                last_images = message.images;
            }
        }
        for (const std::string& image : last_images) {
            if (!json::is_blank(image)) generated_images.push_back(image);
        }
    }

    bool changed = false;
    std::vector<ChatMessage> projected;
    projected.reserve(messages.size());
    for (std::size_t index = 0; index < messages.size(); ++index) {
        ChatMessage next = messages[index];
        if (is_normal_assistant_message(next) && !next.images.empty()) {
            next.images.clear();
            changed = true;
        }
        if (index == latest_user_index && !generated_images.empty()) {
            next.text = add_generated_image_context_note(next.text, generated_images.size());
            std::vector<std::string> merged = generated_images;
            for (const std::string& image : next.images) {
                if (std::find(merged.begin(), merged.end(), image) == merged.end()) {
                    merged.push_back(image);
                }
            }
            next.images = std::move(merged);
            changed = true;
        }
        projected.push_back(std::move(next));
    }
    return changed ? projected : messages;
}

std::vector<ChatMessage> project_tool_result_images_to_user_message(
    const std::vector<ChatMessage>& messages, bool include_images) {
    const bool any_result_images =
        std::any_of(messages.begin(), messages.end(), [](const ChatMessage& message) {
            return starts_with(message.id, kResultMsgPrefix) && !message.images.empty();
        });
    if (!any_result_images) return messages;

    std::vector<ChatMessage> projected;
    std::size_t index = 0;
    while (index < messages.size()) {
        const ChatMessage& message = messages[index];
        projected.push_back(message);
        if (!starts_with(message.id, kResultMsgPrefix)) {
            ++index;
            continue;
        }
        std::vector<ChatMessage> batch{message};
        std::size_t next = index + 1;
        while (next < messages.size() && starts_with(messages[next].id, kResultMsgPrefix)) {
            projected.push_back(messages[next]);
            batch.push_back(messages[next]);
            ++next;
        }
        std::vector<std::string> images;
        for (const ChatMessage& row : batch) {
            for (const std::string& image : row.images) {
                if (json::is_blank(image)) continue;
                if (std::find(images.begin(), images.end(), image) == images.end()) {
                    images.push_back(image);
                }
            }
        }
        if (!images.empty()) {
            std::string seed;
            for (const ChatMessage& row : batch) {
                if (!seed.empty()) seed += ':';
                seed += row.id;
            }
            const std::string description_block = transcription_descriptions_for_batch(batch);
            ChatMessage context_row;
            // The id must not start with tool_/result_/compact_: serializers branch on prefixes.
            context_row.id = std::string(kImageContextPrefix) + digest8_hex(seed);
            context_row.parent_id = batch.back().id;
            if (include_images) {
                std::string text =
                    "[Tool visual result: inspect the attached image";
                text += images.size() == 1 ? " before continuing.]" : "s before continuing.]";
                if (!json::is_blank(description_block)) {
                    text += kImageTranscriptionBlock;
                    text += description_block;
                }
                context_row.text = text;
                context_row.images = images;
            } else if (!json::is_blank(description_block)) {
                context_row.text =
                    "[Tool visual result: the image description follows.]" +
                    std::string(kImageTranscriptionBlock) + description_block;
            } else {
                context_row.text =
                    "[Tool visual result unavailable: the current model does not support image "
                    "input.]";
            }
            context_row.participant = Participant::User;
            context_row.status = MessageStatus::Success;
            context_row.timestamp = batch.back().timestamp;
            context_row.run_id = batch.front().run_id;
            context_row.run_sequence = batch.front().run_sequence;
            projected.push_back(std::move(context_row));
        }
        index = next;
    }
    return projected;
}

std::vector<ChatMessage> project_generation_statuses_for_api(
    const std::vector<ChatMessage>& messages,
    const std::function<std::string(const std::string&)>& generation_error_formatter) {
    const auto is_generation_status_message = [](const ChatMessage& message) {
        return !message.is_tool_protocol_message() && !message.is_context_compact() &&
               (message.participant == Participant::Error ||
                message.status == MessageStatus::Error || message.status == MessageStatus::Stopped);
    };
    const bool any = std::any_of(messages.begin(), messages.end(),
                                 [&](const ChatMessage& message) {
                                     return is_generation_status_message(message);
                                 });
    if (!any) return messages;

    std::vector<ChatMessage> out;
    out.reserve(messages.size());
    for (ChatMessage message : messages) {
        if (!is_generation_status_message(message)) {
            out.push_back(std::move(message));
            continue;
        }
        const bool is_error = message.participant == Participant::Error ||
                              message.status == MessageStatus::Error;
        std::optional<std::string> persisted_error;
        if (message.segments) {
            for (auto iterator = message.segments->rbegin(); iterator != message.segments->rend();
                 ++iterator) {
                if (iterator->type == "error" && !json::is_blank(iterator->content)) {
                    persisted_error = iterator->content;
                    break;
                }
            }
        }
        bool has_persisted_answer = false;
        if (message.segments) {
            for (const MessageSegment& segment : *message.segments) {
                if (segment.type == "answer" && !json::is_blank(segment.content)) {
                    has_persisted_answer = true;
                }
            }
        }
        std::optional<std::string> legacy_error_detail;
        if (is_error && !persisted_error && !has_persisted_answer &&
            !json::is_blank(message.text)) {
            legacy_error_detail = message.text;
        }
        std::optional<std::string> source_detail =
            persisted_error ? persisted_error : legacy_error_detail;
        std::optional<std::string> error_detail;
        if (source_detail) error_detail = generation_error_formatter(*source_detail);

        std::string partial_answer;
        if (!is_error) {
            partial_answer = message.text;
        } else if (legacy_error_detail) {
            partial_answer = "";
        } else if (persisted_error && json::trim(message.text) == json::trim(*persisted_error)) {
            partial_answer = "";
        } else {
            partial_answer = message.text;
        }
        const std::string terminal_text = generation_status_event_text(is_error, error_detail);

        message.text.clear();
        if (!json::is_blank(partial_answer)) message.text = partial_answer;
        if (!json::is_blank(terminal_text)) {
            if (!message.text.empty()) message.text += "\n\n";
            message.text += terminal_text;
        }
        message.images.clear();
        message.thoughts.reset();
        message.thought_title.reset();
        message.token_count = 0;
        message.status = MessageStatus::Success;
        message.participant = Participant::Model;
        message.model_name.reset();
        message.tool_call.reset();
        message.segments.reset();
        message.attachment_meta.reset();
        out.push_back(std::move(message));
    }
    return out;
}

}  // namespace agora::core::prep
