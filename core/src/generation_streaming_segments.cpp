// Port of viewmodel/GenerationStreamingSegments.kt.
//
// The thought timing, the segment merge, the live-segment builder, the citation rebase, the
// bounded output transform and the final snapshot follow the Kotlin source line by line.
#include "agora/core/generation_streaming_segments.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/message_persistence_guard.hpp"

namespace agora::core::generation_streaming {

namespace {

bool is_blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

/** mergeDurationMs(first, second): the sum, but only when it is positive. */
std::optional<std::int64_t> merge_duration_ms(const std::optional<std::int64_t>& first,
                                              const std::optional<std::int64_t>& second) {
    const std::int64_t merged = (first.value_or(0)) + (second.value_or(0));
    if (merged <= 0) return std::nullopt;
    return merged;
}

}  // namespace

ThoughtTiming::ThoughtTiming(Clock clock) : clock_(std::move(clock)) {}

void ThoughtTiming::ensure_started() {
    if (!current_started_at_ms_.has_value()) current_started_at_ms_ = clock_();
}

std::optional<std::int64_t> ThoughtTiming::live_duration_ms() const {
    const std::int64_t live_elapsed =
        current_started_at_ms_.has_value() ? clock_() - *current_started_at_ms_ : 0;
    const std::int64_t total = current_duration_ms_ + live_elapsed;
    if (total <= 0) return std::nullopt;
    return total;
}

void ThoughtTiming::finish_current() {
    if (!current_started_at_ms_.has_value()) return;
    const std::int64_t elapsed = clock_() - *current_started_at_ms_;
    if (elapsed > 0) {
        cumulative_duration_ms_ += elapsed;
        current_duration_ms_ += elapsed;
        total_duration_ms_ = cumulative_duration_ms_;
    }
    current_started_at_ms_.reset();
}

void ThoughtTiming::reset_current_duration() { current_duration_ms_ = 0; }

void ThoughtTiming::adopt_total_duration(const std::optional<std::int64_t>& duration_ms) {
    total_duration_ms_ = duration_ms;
}

prep::MessageStatus status_after_thought_phase_finished(prep::MessageStatus status) {
    return status == prep::MessageStatus::Thinking ? prep::MessageStatus::Sending : status;
}

void append_merged_segment(std::vector<prep::MessageSegment>* target,
                           const prep::MessageSegment& segment) {
    if (target->empty()) {
        target->push_back(segment);
        return;
    }
    const prep::MessageSegment& last = target->back();
    const bool can_merge =
        last.type == segment.type &&
        (segment.type == "answer" ||
         (segment.type == "thought" && !last.signature.has_value() &&
          !segment.signature.has_value()));
    if (!can_merge) {
        target->push_back(segment);
        return;
    }
    prep::MessageSegment merged = last;
    merged.content = last.content + segment.content;
    merged.signature = segment.signature.has_value() ? segment.signature : last.signature;
    merged.signature_provider =
        segment.signature_provider.has_value() ? segment.signature_provider : last.signature_provider;
    merged.duration_ms = merge_duration_ms(last.duration_ms, segment.duration_ms);
    merged.streaming_text_deltas.insert(merged.streaming_text_deltas.end(),
                                        segment.streaming_text_deltas.begin(),
                                        segment.streaming_text_deltas.end());
    target->back() = std::move(merged);
}

citations::CitationRecord rebase_citation_for_final_answer(
    const citations::CitationRecord& citation, std::int32_t provider_answer_start,
    std::string_view final_answer) {
    if (citation.anchors.empty()) return citation;
    citations::CitationRecord shifted = citation;
    shifted.anchors.clear();
    for (const citations::CitationAnchor& anchor : citation.anchors) {
        const std::int64_t start = static_cast<std::int64_t>(provider_answer_start) + anchor.start_index;
        const std::int64_t end = static_cast<std::int64_t>(provider_answer_start) + anchor.end_index;
        if (start < 0 || start > std::numeric_limits<std::int32_t>::max() || end < 0 ||
            end > std::numeric_limits<std::int32_t>::max()) {
            continue;
        }
        citations::CitationAnchor moved = anchor;
        moved.start_index = static_cast<std::int32_t>(start);
        moved.end_index = static_cast<std::int32_t>(end);
        shifted.anchors.push_back(std::move(moved));
    }
    const std::optional<citations::CitationRecord> normalized = citations::policy::normalize_record(
        shifted, std::optional<std::string>(std::string(final_answer)));
    if (normalized.has_value()) return *normalized;
    citations::CitationRecord stripped = citation;
    stripped.anchors.clear();
    return stripped;
}

std::optional<std::vector<prep::MessageSegment>> build_live_segments(
    const std::vector<prep::MessageSegment>& flushed, std::string_view answer, std::string_view thought,
    const std::optional<std::string>& signature, const std::optional<std::string>& signature_provider,
    const std::optional<std::int64_t>& thought_duration_ms,
    const std::optional<std::string>& error_message, const std::optional<std::string>& error_code,
    const std::vector<run_recovery::StreamingTextDelta>& answer_deltas) {
    std::vector<prep::MessageSegment> citations;
    std::vector<prep::MessageSegment> result;
    for (const prep::MessageSegment& segment : flushed) {
        if (segment.type == "citation") {
            citations.push_back(segment);
        } else {
            result.push_back(segment);
        }
    }
    if (!answer.empty()) {
        prep::MessageSegment segment;
        segment.type = "answer";
        segment.content = std::string(answer);
        segment.streaming_text_deltas = answer_deltas;
        append_merged_segment(&result, segment);
    }
    if (!thought.empty()) {
        prep::MessageSegment segment;
        segment.type = "thought";
        segment.content = std::string(thought);
        segment.signature = signature;
        segment.signature_provider = signature_provider;
        segment.duration_ms = thought_duration_ms;
        append_merged_segment(&result, segment);
    }
    if (error_message.has_value() && !is_blank(*error_message)) {
        prep::MessageSegment segment;
        segment.type = "error";
        segment.content = *error_message;
        segment.error_code = error_code;
        result.push_back(std::move(segment));
    }
    result.insert(result.end(), citations.begin(), citations.end());
    if (result.empty()) return std::nullopt;
    return result;
}

std::optional<std::string> terminal_generation_error_message(
    prep::MessageStatus status, const std::optional<std::string>& current_error,
    std::string_view fallback_error) {
    if (status == prep::MessageStatus::Error) {
        if (current_error.has_value() && !is_blank(*current_error)) return current_error;
        return std::string(fallback_error);
    }
    return current_error;
}

prep::ChatMessage with_bounded_output_text_transform(
    const prep::ChatMessage& message,
    const std::function<std::string(const std::string&, prep::MessageStatus)>& transform) {
    const std::string transformed_text = message_persistence_guard::clip_text(
        transform(message.text, message.status));
    if (transformed_text == message.text) return message;
    int answer_segment_count = 0;
    if (message.segments.has_value()) {
        for (const prep::MessageSegment& segment : *message.segments) {
            if (segment.type == "answer") ++answer_segment_count;
        }
    }
    int answer_segment_index = 0;
    std::size_t transformed_cursor = 0;
    prep::ChatMessage out = message;
    if (message.segments.has_value()) {
        std::vector<prep::MessageSegment> transformed;
        transformed.reserve(message.segments->size());
        for (const prep::MessageSegment& segment : *message.segments) {
            if (segment.type != "answer") {
                transformed.push_back(segment);
                continue;
            }
            ++answer_segment_index;
            prep::MessageSegment copy = segment;
            if (answer_segment_index == answer_segment_count) {
                const std::size_t start = std::min<std::size_t>(transformed_cursor,
                                                               transformed_text.size());
                copy.content = transformed_text.substr(start);
            } else {
                const std::size_t end = std::min<std::size_t>(
                    transformed_cursor + segment.content.size(), transformed_text.size());
                copy.content = transformed_text.substr(transformed_cursor, end - transformed_cursor);
                transformed_cursor = end;
            }
            copy.streaming_text_deltas.clear();
            transformed.push_back(std::move(copy));
        }
        out.segments = std::move(transformed);
    }
    out.text = transformed_text;
    return out;
}

prep::ChatMessage to_message(const FinalSnapshot& snapshot) {
    prep::ChatMessage message;
    message.id = snapshot.message_id;
    message.parent_id = snapshot.parent_id;
    message.text = message_persistence_guard::clip_text(snapshot.text);
    message.images = snapshot.images;
    message.thoughts = is_blank(snapshot.thoughts) ? std::nullopt
                                                   : std::optional<std::string>(snapshot.thoughts);
    message.thought_title = snapshot.thought_title;
    message.token_count = snapshot.token_count;
    message.token_usage = snapshot.token_usage;
    message.status = snapshot.status;
    message.participant = prep::Participant::Model;
    message.timestamp = snapshot.timestamp;
    message.thought_time_ms = snapshot.thought_time_ms;
    message.model_name = snapshot.model_name;
    const std::optional<std::vector<prep::MessageSegment>> live = build_live_segments(
        snapshot.flushed_segments, snapshot.answer_buffer, snapshot.thought_buffer,
        snapshot.thought_signature, snapshot.thought_signature_provider, snapshot.thought_duration_ms,
        snapshot.error_message, snapshot.error_code, snapshot.answer_deltas);
    if (live.has_value()) {
        message.segments = live;
    } else if (!snapshot.flushed_segments.empty()) {
        message.segments = snapshot.flushed_segments;
    }
    message.run_id = snapshot.run_id;
    message.run_sequence = snapshot.run_sequence;
    return message;
}

}  // namespace agora::core::generation_streaming
