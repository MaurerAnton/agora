#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/citations.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/token_usage.hpp"

namespace agora::core::generation_streaming {

// The chat model, the segment model and the citation policy are already-ported modules.
namespace prep = agora::core::prep;
namespace citations = agora::core::citations;

/**
 * Port of viewmodel/GenerationStreamingSegments.kt: the live segment timeline of a generation.
 *
 * The policy file is lifted verbatim by the golden stand (only the visibility is widened), so this
 * port follows it line by line. The chat model and the segment model are the already-ported
 * message_preparation types; the citation policy and the persistence guard are reused as they are.
 */

/** GenerationThoughtTiming: the duration bookkeeping of the current thought phase. */
class ThoughtTiming {
public:
    using Clock = std::function<std::int64_t()>;

    explicit ThoughtTiming(Clock clock);

    void ensure_started();
    std::optional<std::int64_t> live_duration_ms() const;
    void finish_current();
    void reset_current_duration();
    void adopt_total_duration(const std::optional<std::int64_t>& duration_ms);

    std::int64_t current_duration_ms() const { return current_duration_ms_; }
    const std::optional<std::int64_t>& total_duration_ms() const { return total_duration_ms_; }

private:
    Clock clock_;
    std::int64_t cumulative_duration_ms_ = 0;
    std::optional<std::int64_t> current_started_at_ms_;
    std::int64_t current_duration_ms_ = 0;
    std::optional<std::int64_t> total_duration_ms_;
};

/** statusAfterThoughtPhaseFinished(status). */
prep::MessageStatus status_after_thought_phase_finished(
    prep::MessageStatus status);

/** appendMergedSegment(target, segment). */
void append_merged_segment(std::vector<prep::MessageSegment>* target,
                           const prep::MessageSegment& segment);

/** rebaseCitationForFinalAnswer(citation, providerAnswerStart, finalAnswer). */
citations::CitationRecord rebase_citation_for_final_answer(
    const citations::CitationRecord& citation, std::int32_t provider_answer_start,
    std::string_view final_answer);

/** buildLiveSegments(flushed, answer, thought, ...). */
std::optional<std::vector<prep::MessageSegment>> build_live_segments(
    const std::vector<prep::MessageSegment>& flushed, std::string_view answer,
    std::string_view thought, const std::optional<std::string>& signature = std::nullopt,
    const std::optional<std::string>& signature_provider = std::nullopt,
    const std::optional<std::int64_t>& thought_duration_ms = std::nullopt,
    const std::optional<std::string>& error_message = std::nullopt,
    const std::optional<std::string>& error_code = std::nullopt,
    const std::vector<run_recovery::StreamingTextDelta>& answer_deltas = {});

/** terminalGenerationErrorMessage(status, currentError, fallbackError). */
std::optional<std::string> terminal_generation_error_message(
    prep::MessageStatus status, const std::optional<std::string>& current_error,
    std::string_view fallback_error);

/** ChatMessage.withBoundedOutputTextTransform(transform). */
prep::ChatMessage with_bounded_output_text_transform(
    const prep::ChatMessage& message,
    const std::function<std::string(const std::string&, prep::MessageStatus)>&
        transform);

/** viewmodel/GenerationStreamingSegments.kt::GenerationFinalSnapshot. */
struct FinalSnapshot {
    std::string message_id;
    std::optional<std::string> parent_id;
    std::string text;
    std::vector<std::string> images;
    std::string thoughts;
    std::optional<std::string> thought_title;
    int token_count = 0;
    std::optional<TokenUsage> token_usage;
    prep::MessageStatus status = prep::MessageStatus::Success;
    std::int64_t timestamp = 0;
    std::optional<std::int64_t> thought_time_ms;
    std::string model_name;
    std::vector<prep::MessageSegment> flushed_segments;
    std::string answer_buffer;
    std::string thought_buffer;
    std::optional<std::string> thought_signature;
    std::optional<std::string> thought_signature_provider;
    std::optional<std::int64_t> thought_duration_ms;
    std::optional<std::string> error_message;
    std::optional<std::string> error_code;
    std::string run_id;
    std::int64_t run_sequence = 0;
    std::vector<run_recovery::StreamingTextDelta> answer_deltas;
};

/** GenerationFinalSnapshot.toMessage(). */
prep::ChatMessage to_message(const FinalSnapshot& snapshot);

}  // namespace agora::core::generation_streaming
