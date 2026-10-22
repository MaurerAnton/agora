// Port of viewmodel/GenerationPolicies.kt and viewmodel/AutomaticMessageApiProjection.kt.
//
// The policies are followed line by line: the terminal disposition rules, the two throttle gates,
// the thought-segment tools, the message templates (with the JVM's time formats) and the API-only
// history projection.
#include "agora/core/generation_policies.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <limits>
#include <vector>

namespace agora::core::generation_policy {

namespace {

constexpr std::string_view kToolMsgPrefix = "tool_";
constexpr std::string_view kResultMsgPrefix = "result_";
constexpr std::string_view kCompactMsgPrefix = "compact_";
constexpr std::string_view kContextSummaryPrefix = "context_summary_";
constexpr std::string_view kApiInitialUserPrefix = "api_initial_user_";
constexpr std::string_view kApiCompactContinuationPrefix = "api_compact_continuation_";

bool is_blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// ------------------------------------------------------------------ civil time (UTC)

/** Days from 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm). */
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct CivilTime {
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int weekday = 4;  // 1970-01-01 was a Thursday
};

CivilTime civil_from_millis(std::int64_t millis) {
    std::int64_t seconds = millis / 1000;
    std::int64_t remainder = millis % 1000;
    if (remainder < 0) {
        remainder += 1000;
        --seconds;
    }
    std::int64_t days = seconds / 86400;
    std::int64_t day_seconds = seconds % 86400;
    if (day_seconds < 0) {
        day_seconds += 86400;
        --days;
    }
    CivilTime out;
    out.hour = static_cast<int>(day_seconds / 3600);
    out.minute = static_cast<int>((day_seconds % 3600) / 60);
    out.second = static_cast<int>(day_seconds % 60);
    out.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 -> Thursday (4)

    std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp + (mp < 10 ? 3 : -9);
    year += (month <= 2);
    out.year = static_cast<int>(year);
    out.month = month;
    out.day = day;
    return out;
}

std::string two_digits(int value) {
    std::string out = std::to_string(value < 0 ? -value : value);
    if (out.size() < 2) out.insert(out.begin(), '0');
    return out;
}

std::string four_digits(int value) {
    std::string out = std::to_string(value < 0 ? -value : value);
    while (out.size() < 4) out.insert(out.begin(), '0');
    return out;
}

/** SimpleDateFormat("HH:mm:ss"). */
std::string format_time_of_day(std::int64_t millis) {
    const CivilTime time = civil_from_millis(millis);
    return two_digits(time.hour) + ":" + two_digits(time.minute) + ":" + two_digits(time.second);
}

/** SimpleDateFormat("yyyy-MM-dd EEE") with the US locale. */
std::string format_sent_date(std::int64_t millis) {
    const CivilTime time = civil_from_millis(millis);
    static const char* const kWeekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return four_digits(time.year) + "-" + two_digits(static_cast<int>(time.month)) + "-" +
           two_digits(static_cast<int>(time.day)) + " " + kWeekdays[time.weekday % 7];
}

void replace_all(std::string* text, std::string_view needle, const std::string& replacement) {
    if (needle.empty()) return;
    std::size_t position = 0;
    while ((position = text->find(needle, position)) != std::string::npos) {
        text->replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

/** The data-class equality of MessageSegment (Kotlin compares every declared field). */
bool same_segment(const prep::MessageSegment& left, const prep::MessageSegment& right) {
    return left.type == right.type && left.content == right.content &&
           left.tool_name == right.tool_name && left.tool_args == right.tool_args &&
           left.tool_result == right.tool_result && left.tool_call_id == right.tool_call_id &&
           left.signature == right.signature && left.signature_provider == right.signature_provider &&
           left.tool_transcription == right.tool_transcription &&
           left.error_code == right.error_code &&
           left.response_output_item_provider == right.response_output_item_provider &&
           left.response_output_items.size() == right.response_output_items.size();
}

std::string xml_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string element(std::string_view name, std::string_view text) {
    return "<" + std::string(name) + ">\n" + xml_escape(text) + "\n</" + std::string(name) + ">";
}

}  // namespace

bool should_post_generation_terminal_notification(prep::MessageStatus message_status,
                                                 bool has_pending_guidance, bool is_context_compact,
                                                 bool app_in_foreground,
                                                 const std::optional<bool>& conversation_visible) {
    if (!conversation_visible.has_value()) {
        return !app_in_foreground && message_status == prep::MessageStatus::Success &&
               !has_pending_guidance;
    }
    const bool terminal_needs_attention =
        message_status == prep::MessageStatus::Error ||
        (message_status == prep::MessageStatus::Success && !has_pending_guidance);
    return !*conversation_visible && terminal_needs_attention &&
           (!is_context_compact || message_status == prep::MessageStatus::Error);
}

TerminalDisposition generation_terminal_disposition(prep::MessageStatus message_status,
                                                    bool has_pending_guidance,
                                                    const std::optional<bool>& conversation_visible) {
    TerminalDisposition disposition;
    if (message_status == prep::MessageStatus::Stopped) {
        disposition.run_status = runtime::RunStatus::Stopped;
        disposition.end_reason = runtime::RunEndReason::UserStopped;
        disposition.mark_conversation_unread = false;
        return disposition;
    }
    if (message_status == prep::MessageStatus::Error) {
        disposition.run_status = runtime::RunStatus::Failed;
        disposition.end_reason = runtime::RunEndReason::ProviderError;
        disposition.mark_conversation_unread =
            conversation_visible.has_value() && !*conversation_visible;
        return disposition;
    }
    disposition.run_status = runtime::RunStatus::Completed;
    disposition.end_reason = runtime::RunEndReason::ModelCompleted;
    disposition.mark_conversation_unread =
        !has_pending_guidance && (!conversation_visible.has_value() || !*conversation_visible);
    return disposition;
}

StreamingCheckpointGate::StreamingCheckpointGate(std::int64_t interval_ms)
    : interval_ms_(interval_ms) {}

bool StreamingCheckpointGate::should_checkpoint(std::int64_t now_ms, bool force) {
    if (!force && last_checkpoint_at_.has_value() && now_ms >= *last_checkpoint_at_ &&
        now_ms - *last_checkpoint_at_ < interval_ms_) {
        return false;
    }
    last_checkpoint_at_ = now_ms;
    return true;
}

StreamingUiUpdateGate::StreamingUiUpdateGate(std::int64_t interval_ms) : interval_ms_(interval_ms) {}

bool StreamingUiUpdateGate::is_due(std::int64_t now_ms) {
    if (!last_published_at_.has_value()) return true;
    const std::int64_t previous = *last_published_at_;
    return now_ms < previous || now_ms - previous >= interval_ms_;
}

void StreamingUiUpdateGate::record_published(std::int64_t now_ms) { last_published_at_ = now_ms; }

void StreamingUiUpdateGate::reset() { last_published_at_.reset(); }

std::vector<prep::MessageSegment> tool_round_thought_segments(
    const std::vector<prep::MessageSegment>& segments, int from_index) {
    const int size = static_cast<int>(segments.size());
    const int safe_start = std::max(0, std::min(from_index, size));
    std::vector<prep::MessageSegment> out;
    for (std::size_t index = static_cast<std::size_t>(safe_start); index < segments.size(); ++index) {
        if (segments[index].type == "thought") out.push_back(segments[index]);
    }
    return out;
}

std::vector<prep::MessageSegment> ToolRoundHistoryCompactor::compact(
    const std::string& run_id, const std::vector<prep::MessageSegment>& segments) {
    std::vector<prep::MessageSegment> thoughts;
    for (const prep::MessageSegment& segment : segments) {
        if (segment.type == "thought") thoughts.push_back(segment);
    }
    if (thoughts.empty()) return segments;

    std::vector<prep::MessageSegment> history;
    for (const auto& entry : history_) {
        if (entry.first == run_id) {
            history = entry.second;
            break;
        }
    }

    std::size_t repeated_prefix_size = 0;
    if (!history.empty() && thoughts.size() > history.size()) {
        bool prefix_matches = true;
        for (std::size_t index = 0; index < history.size(); ++index) {
            if (!same_segment(history[index], thoughts[index])) {
                prefix_matches = false;
                break;
            }
        }
        if (prefix_matches) repeated_prefix_size = history.size();
    }

    std::vector<prep::MessageSegment> updated;
    if (repeated_prefix_size > 0) {
        updated = thoughts;
    } else if (history.empty()) {
        updated = thoughts;
    } else if (thoughts.size() == history.size() &&
               std::equal(history.begin(), history.end(), thoughts.begin(),
                          [](const prep::MessageSegment& left, const prep::MessageSegment& right) {
                              return same_segment(left, right);
                          })) {
        updated = history;
    } else {
        updated = history;
        updated.insert(updated.end(), thoughts.begin(), thoughts.end());
    }
    bool stored = false;
    for (auto& entry : history_) {
        if (entry.first == run_id) {
            entry.second = updated;
            stored = true;
            break;
        }
    }
    if (!stored) history_.emplace_back(run_id, updated);

    if (repeated_prefix_size == 0) return segments;

    std::size_t to_drop = repeated_prefix_size;
    std::vector<prep::MessageSegment> out;
    for (const prep::MessageSegment& segment : segments) {
        if (segment.type == "thought" && to_drop > 0) {
            --to_drop;
            continue;
        }
        out.push_back(segment);
    }
    return out;
}

std::vector<prep::ChatMessage> apply_message_templates_to_messages(
    const std::vector<prep::ChatMessage>& messages, const std::optional<std::string>& user_prepend,
    const std::optional<std::string>& user_postpend, const std::optional<std::string>& assistant_prepend,
    const std::optional<std::string>& assistant_postpend) {
    if (!user_prepend.has_value() && !user_postpend.has_value() && !assistant_prepend.has_value() &&
        !assistant_postpend.has_value()) {
        return messages;
    }
    std::vector<prep::ChatMessage> out;
    out.reserve(messages.size());
    for (const prep::ChatMessage& message : messages) {
        const bool is_special_message =
            starts_with(message.id, kToolMsgPrefix) || starts_with(message.id, kResultMsgPrefix) ||
            starts_with(message.id, kCompactMsgPrefix) ||
            starts_with(message.id, kContextSummaryPrefix) ||
            starts_with(message.id, kApiInitialUserPrefix) ||
            starts_with(message.id, kApiCompactContinuationPrefix);
        const std::string* before = nullptr;
        const std::string* after = nullptr;
        if (!is_special_message && !message.text.empty()) {
            if (message.participant == prep::Participant::User) {
                before = user_prepend.has_value() ? &*user_prepend : nullptr;
                after = user_postpend.has_value() ? &*user_postpend : nullptr;
            } else if (message.participant == prep::Participant::Model) {
                before = assistant_prepend.has_value() ? &*assistant_prepend : nullptr;
                after = assistant_postpend.has_value() ? &*assistant_postpend : nullptr;
            }
        }
        if (before == nullptr && after == nullptr) {
            out.push_back(message);
            continue;
        }
        std::string message_model_id;
        if (message.model_name.has_value() && !is_blank(*message.model_name)) {
            message_model_id = prep::parse_prefixed_model_id(*message.model_name).model_name;
        }
        const auto resolve = [&](const std::string* value) {
            std::string text = value != nullptr ? *value : std::string();
            replace_all(&text, "{sent_time}", format_time_of_day(message.timestamp));
            replace_all(&text, "{sent_date}", format_sent_date(message.timestamp));
            replace_all(&text, "{message_model_id}", message_model_id);
            return text;
        };
        const std::string resolved_before = resolve(before);
        const std::string resolved_after = resolve(after);
        if (resolved_before.empty() && resolved_after.empty()) {
            out.push_back(message);
            continue;
        }
        prep::ChatMessage copy = message;
        copy.text = resolved_before + message.text + resolved_after;
        out.push_back(std::move(copy));
    }
    return out;
}

std::vector<prep::ChatMessage> apply_user_template_to_messages(
    const std::vector<prep::ChatMessage>& messages, const std::optional<std::string>& prepend,
    const std::optional<std::string>& postpend) {
    return apply_message_templates_to_messages(messages, prepend, postpend, std::nullopt,
                                               std::nullopt);
}

std::string automatic_message_api_text(const source_model::Source& source, std::string_view text) {
    switch (source.kind) {
        case source_model::Kind::Task:
            return element("task_prompt", text);
        case source_model::Kind::Loop:
            return element("loop_prompt", text);
        case source_model::Kind::AskUser: {
            std::string out = "<ask_user_answers>\n";
            for (const source_model::AskUserItem& item : source.ask_user) {
                out += "<item>\n";
                out += "<question>" + xml_escape(item.question) + "</question>\n";
                if (item.answer.has_value()) {
                    out += "<answer>" + xml_escape(*item.answer) + "</answer>\n";
                } else {
                    out += "<unanswered/>\n";
                }
                out += "</item>\n";
            }
            out += "</ask_user_answers>";
            return out;
        }
    }
    return std::string(text);
}

std::vector<prep::ChatMessage> project_automatic_messages_for_api(
    const std::vector<prep::ChatMessage>& messages) {
    std::vector<prep::ChatMessage> out;
    out.reserve(messages.size());
    for (const prep::ChatMessage& message : messages) {
        if (!message.source.has_value() || message.participant != prep::Participant::User) {
            out.push_back(message);
            continue;
        }
        prep::ChatMessage copy = message;
        copy.text = automatic_message_api_text(*message.source, message.text);
        out.push_back(std::move(copy));
    }
    return out;
}

std::vector<prep::ChatMessage> project_generation_input_messages(
    const std::vector<prep::ChatMessage>& messages, bool include_images,
    const std::optional<std::string>& user_prepend, const std::optional<std::string>& user_postpend,
    const std::optional<std::string>& assistant_prepend, const std::optional<std::string>& assistant_postpend,
    const std::optional<std::string>& initial_user_prompt) {
    std::vector<prep::ChatMessage> projected =
        prep::project_tool_result_images_to_user_message(
            prep::project_assistant_images_to_latest_user_message(
                project_automatic_messages_for_api(messages), include_images),
            include_images);
    projected = apply_message_templates_to_messages(projected, user_prepend, user_postpend,
                                                    assistant_prepend, assistant_postpend);
    if (!include_images) {
        for (prep::ChatMessage& message : projected) {
            if (!message.images.empty()) message.images.clear();
        }
    }
    if (!initial_user_prompt.has_value() || is_blank(*initial_user_prompt)) return projected;

    prep::ChatMessage initial;
    initial.id = std::string(kApiInitialUserPrefix) +
                  (projected.empty() ? std::string() : projected.back().id);
    if (!projected.empty()) {
        initial.parent_id = projected.back().id;
        initial.timestamp = projected.back().timestamp == std::numeric_limits<std::int64_t>::max()
                                ? projected.back().timestamp
                                : projected.back().timestamp + 1;
    } else {
        initial.timestamp = 0;
    }
    initial.text = *initial_user_prompt;
    initial.participant = prep::Participant::User;
    projected.push_back(std::move(initial));
    return projected;
}

}  // namespace agora::core::generation_policy
