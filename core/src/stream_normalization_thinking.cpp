// IncrementalThinkingParser, its facades, and findImplicitThinkingClose.
//
// The Kotlin source keeps marker strings split ("<tool_" + "call>" style) so the bare protocol
// literals never appear as contiguous substrings in source tooling; the C++ port uses adjacent
// string-literal concatenation for the same reason (identical bytes after compilation).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/stream_normalization.hpp"
#include "stream_normalization_internal.hpp"

namespace agora::core::stream_normalization {

namespace unicode = agora::core::unicode;

// ---------------------------------------------------------------------------------------------
// Marker tables.

constexpr std::string_view kCloseThink = "<" "/think>";
constexpr std::string_view kCloseThinking = "<" "/thinking>";
constexpr std::string_view kCloseReasoning = "<" "/reasoning>";
constexpr std::string_view kCloseAnalysis = "<" "/analysis>";
constexpr std::string_view kCloseThought = "<" "/thought>";

struct IncrementalThinkingParser::Marker {
    std::string start;
    std::vector<std::string> ends;
    bool implicit = false;
};

namespace {

std::vector<std::string> channel_ends() {
    return {
        "<|end|>",
        "<|channel|>final<|message|>",
        "<|start|>assistant<|channel|>final<|message|>",
    };
}

const std::vector<IncrementalThinkingParser::Marker>& explicit_markers() {
    static const std::vector<IncrementalThinkingParser::Marker> kMarkers = [] {
        std::vector<IncrementalThinkingParser::Marker> markers;
        const auto add = [&](std::string start, std::vector<std::string> ends) {
            markers.push_back(IncrementalThinkingParser::Marker{
                std::move(start),
                std::move(ends),
                false,
            });
        };
        add("<think>", {std::string(kCloseThink)});
        add("<thinking>", {std::string(kCloseThinking)});
        add("<reasoning>", {std::string(kCloseReasoning)});
        add("<analysis>", {std::string(kCloseAnalysis)});
        add("<thought>", {std::string(kCloseThought)});
        add("<|channel|>thought<|message|>", channel_ends());
        add("<|channel|>reasoning<|message|>", channel_ends());
        add("<|channel|>analysis<|message|>", channel_ends());
        add("<|start|>assistant<|channel|>thought<|message|>", channel_ends());
        add("<|start|>assistant<|channel|>reasoning<|message|>", channel_ends());
        add("<|start|>assistant<|channel|>analysis<|message|>", channel_ends());
        // Legacy local templates seen in older llama.cpp model cards.
        add("<|channel>thought\n", {"<channel|>"});
        add("<|channel>analysis\n", {"<channel|>"});
        add("<|channel>thought ", {"<channel|>"});
        add("<|channel>analysis ", {"<channel|>"});
        return markers;
    }();
    return kMarkers;
}

const IncrementalThinkingParser::Marker& implicit_marker() {
    static const IncrementalThinkingParser::Marker kMarker = [] {
        IncrementalThinkingParser::Marker marker;
        marker.start = "";
        marker.ends = {
            std::string(kCloseThink),
            std::string(kCloseThinking),
            std::string(kCloseReasoning),
            std::string(kCloseAnalysis),
            std::string(kCloseThought),
            "<|end|>",
            "<|channel|>final<|message|>",
            "<|start|>assistant<|channel|>final<|message|>",
            "<channel|>",
        };
        marker.implicit = true;
        return marker;
    }();
    return kMarker;
}

enum class MatchKind { kNone, kPartial, kFull };

MatchKind marker_match(std::string_view remainder, std::string_view marker) {
    if (detail::starts_with_ci(remainder, marker)) return MatchKind::kFull;
    if (remainder.size() < marker.size() && detail::starts_with_ci(marker, remainder)) {
        return MatchKind::kPartial;
    }
    return MatchKind::kNone;
}

struct EndMatch {
    std::optional<std::size_t> length;
    bool partial = false;
};

EndMatch end_match(std::string_view remainder, const std::vector<std::string>& endings) {
    bool partial = false;
    for (const std::string& ending : endings) {
        switch (marker_match(remainder, ending)) {
            case MatchKind::kFull:
                return EndMatch{ending.size(), false};
            case MatchKind::kPartial:
                partial = true;
                break;
            case MatchKind::kNone:
                break;
        }
    }
    return EndMatch{std::nullopt, partial};
}

struct StartMatch {
    const IncrementalThinkingParser::Marker* marker = nullptr;
    bool partial = false;
};

StartMatch find_start_match(std::string_view remainder) {
    for (const auto& marker : explicit_markers()) {
        switch (marker_match(remainder, marker.start)) {
            case MatchKind::kFull:
                return StartMatch{&marker, false};
            case MatchKind::kPartial:
                return StartMatch{nullptr, true};
            case MatchKind::kNone:
                break;
        }
    }
    return StartMatch{};
}

/** One Markdown delimiter run and the code state it leaves behind. */
struct CodeRun {
    std::size_t length = 0;
    int next_kind = 0;  // 0 none, 1 inline, 2 fence
    char next_fence_character = 0;
    int next_fence_length = 0;
    int next_ticks = 0;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// IncrementalThinkingParser.

IncrementalThinkingParser::IncrementalThinkingParser(bool start_in_thinking)
    : start_in_thinking_(start_in_thinking) {
    seed_implicit_marker();
}

bool IncrementalThinkingParser::in_thinking() const {
    return !marker_stack_.empty();
}

const std::string& IncrementalThinkingParser::pending_content() const {
    return pending_;
}

void IncrementalThinkingParser::seed_implicit_marker() {
    if (start_in_thinking_) marker_stack_.push_back(&implicit_marker());
}

void IncrementalThinkingParser::feed(
    const std::string& content,
    bool thinking_enabled,
    const TextSink& on_text,
    const TextSink& on_thought) {
    if (content.empty()) return;
    last_thinking_enabled_ = thinking_enabled;
    pending_ += content;
    drain(false, thinking_enabled, on_text, on_thought);
}

void IncrementalThinkingParser::flush(const TextSink& on_text, const TextSink& on_thought) {
    drain(true, last_thinking_enabled_, on_text, on_thought);
    pending_.clear();
}

void IncrementalThinkingParser::flush(
    bool thinking_enabled,
    const TextSink& on_text,
    const TextSink& on_thought) {
    drain(true, thinking_enabled, on_text, on_thought);
    pending_.clear();
}

void IncrementalThinkingParser::reset() {
    marker_stack_.clear();
    implicit_boundary_closed_ = false;
    seed_implicit_marker();
    code_state_kind_ = 0;
    code_fence_character_ = 0;
    code_fence_length_ = 0;
    code_ticks_ = 0;
    pending_.clear();
    line_prefix_ = true;
    line_indent_ = 0;
    indented_code_line_ = false;
    last_thinking_enabled_ = true;
}

void IncrementalThinkingParser::drain(
    bool final,
    bool thinking_enabled,
    const TextSink& on_text,
    const TextSink& on_thought) {
    std::string text;
    std::string thought;
    std::size_t index = 0;

    const auto emit_text = [&]() {
        if (!text.empty()) {
            on_text(text);
            text.clear();
        }
    };
    const auto emit_thought = [&]() {
        if (!thought.empty()) {
            if (thinking_enabled) on_thought(thought);
            thought.clear();
        }
    };
    const auto in_thinking_now = [&]() { return !marker_stack_.empty(); };
    const auto switch_destination = [&]() {
        if (in_thinking_now()) {
            emit_text();
        } else {
            emit_thought();
        }
    };

    const auto update_line_state = [&](std::uint32_t code_point) {
        if (code_point == '\n') {
            line_prefix_ = true;
            line_indent_ = 0;
            indented_code_line_ = false;
        } else if (line_prefix_ && code_point == ' ' && line_indent_ < 4) {
            ++line_indent_;
            if (line_indent_ == 4) indented_code_line_ = true;
        } else if (line_prefix_ && code_point == '\t') {
            indented_code_line_ = true;
            line_prefix_ = false;
        } else {
            line_prefix_ = false;
        }
    };

    const auto append_literal = [&](std::string_view value) {
        if (in_thinking_now()) {
            thought += value;
        } else {
            text += value;
        }
        std::size_t scan = 0;
        while (scan < value.size()) {
            const auto decoded = unicode::decode_utf8(value.data() + scan, value.size() - scan);
            update_line_state(decoded.code_point);
            scan += decoded.length;
        }
    };

    const auto count_run = [&](std::size_t at, char character) {
        std::size_t end = at;
        while (end < pending_.size() && pending_[end] == character) ++end;
        return end - at;
    };

    const auto markdown_delimiter_run = [&](std::size_t at, bool is_final) -> std::optional<CodeRun> {
        const char character = pending_[at];
        if (code_state_kind_ == 0) {
            if (indented_code_line_) return std::nullopt;
            if (character != '`' && character != '~') return std::nullopt;
            const std::size_t run = count_run(at, character);
            if (!is_final && at + run == pending_.size()) return std::nullopt;
            if (line_prefix_ && line_indent_ <= 3 && run >= 3) {
                return CodeRun{run, 2, character, static_cast<int>(run), 0};
            }
            if (character == '`') return CodeRun{run, 1, 0, 0, static_cast<int>(run)};
            return std::nullopt;
        }
        if (code_state_kind_ == 1) {
            if (character != '`') return std::nullopt;
            const std::size_t run = count_run(at, character);
            if (!is_final && at + run == pending_.size()) return std::nullopt;
            if (static_cast<int>(run) == code_ticks_) return CodeRun{run, 0, 0, 0, 0};
            return CodeRun{run, 1, 0, 0, code_ticks_};
        }
        if (!line_prefix_ || line_indent_ > 3 || character != code_fence_character_) {
            return std::nullopt;
        }
        const std::size_t run = count_run(at, character);
        if (!is_final && at + run == pending_.size()) return std::nullopt;
        if (static_cast<int>(run) >= code_fence_length_) return CodeRun{run, 0, 0, 0, 0};
        return CodeRun{run, 2, code_fence_character_, code_fence_length_, 0};
    };

    const auto is_potential_markdown_delimiter = [&](std::size_t at) -> bool {
        if (indented_code_line_) return false;
        const char character = pending_[at];
        if (code_state_kind_ == 0) {
            return character == '`' || (line_prefix_ && character == '~');
        }
        if (code_state_kind_ == 1) {
            return character == '`';
        }
        return line_prefix_ && line_indent_ <= 3 && character == code_fence_character_;
    };

    const auto reset_markdown_state_at_segment_boundary = [&]() {
        code_state_kind_ = 0;
        line_prefix_ = true;
        line_indent_ = 0;
        indented_code_line_ = false;
    };

    while (index < pending_.size()) {
        const std::string_view remainder(pending_.data() + index, pending_.size() - index);

        if (code_state_kind_ == 0 && !indented_code_line_) {
            const Marker* closing = marker_stack_.empty() ? nullptr : marker_stack_.back();
            if (closing != nullptr) {
                const EndMatch close_match = end_match(remainder, closing->ends);
                if (close_match.partial && !final) break;
                if (close_match.length.has_value()) {
                    emit_thought();
                    if (closing->implicit) implicit_boundary_closed_ = true;
                    marker_stack_.pop_back();
                    index += *close_match.length;
                    reset_markdown_state_at_segment_boundary();
                    switch_destination();
                    continue;
                }
                const StartMatch nested = find_start_match(remainder);
                if (nested.partial && !final) break;
                if (nested.marker != nullptr) {
                    if (!closing->implicit) marker_stack_.push_back(nested.marker);
                    index += nested.marker->start.size();
                    reset_markdown_state_at_segment_boundary();
                    continue;
                }
            } else if (!implicit_boundary_closed_) {
                const StartMatch start = find_start_match(remainder);
                if (start.partial && !final) break;
                if (start.marker != nullptr) {
                    emit_text();
                    marker_stack_.push_back(start.marker);
                    index += start.marker->start.size();
                    reset_markdown_state_at_segment_boundary();
                    continue;
                }
            }
        }

        const std::optional<CodeRun> code_run = markdown_delimiter_run(index, final);
        if (!code_run.has_value() && !final && is_potential_markdown_delimiter(index)) break;
        if (code_run.has_value()) {
            append_literal(std::string_view(pending_).substr(index, code_run->length));
            code_state_kind_ = code_run->next_kind;
            code_fence_character_ = code_run->next_fence_character;
            code_fence_length_ = code_run->next_fence_length;
            code_ticks_ = code_run->next_ticks;
            index += code_run->length;
            continue;
        }

        const std::size_t code_point_length =
            detail::utf8_sequence_length(pending_.data() + index, pending_.size() - index);
        append_literal(std::string_view(pending_).substr(index, code_point_length));
        index += code_point_length;
    }

    emit_text();
    emit_thought();
    pending_.erase(0, index);
    if (final && !pending_.empty()) {
        if (in_thinking_now()) {
            if (thinking_enabled) on_thought(pending_);
        } else {
            on_text(pending_);
        }
        std::size_t scan = 0;
        while (scan < pending_.size()) {
            const auto decoded = unicode::decode_utf8(pending_.data() + scan, pending_.size() - scan);
            update_line_state(decoded.code_point);
            scan += decoded.length;
        }
        pending_.clear();
    }
}

// ---------------------------------------------------------------------------------------------
// Facades.

void ThinkingParser::feed(
    const std::string& content,
    bool thinking_enabled,
    const TextSink& on_text,
    const TextSink& on_thought) {
    parser_.feed(content, thinking_enabled, on_text, on_thought);
}

void ThinkingParser::flush(const TextSink& on_text, const TextSink& on_thought) {
    parser_.flush(on_text, on_thought);
}

void ThinkingParser::reset() {
    parser_.reset();
}

bool StreamingThinkTagParser::in_thinking_block() const {
    return parser_.in_thinking();
}

std::string StreamingThinkTagParser::pending_buffer() const {
    return parser_.pending_content();
}

void StreamingThinkTagParser::feed(
    const std::string& content,
    bool thinking_enabled,
    const TextSink& on_text,
    const TextSink& on_thought) {
    parser_.feed(content, thinking_enabled, on_text, on_thought);
}

void StreamingThinkTagParser::flush(
    const TextSink& on_text,
    const TextSink& on_thought,
    std::optional<bool> thinking_enabled) {
    if (!thinking_enabled.has_value()) {
        parser_.flush(on_text, on_thought);
    } else {
        parser_.flush(*thinking_enabled, on_text, on_thought);
    }
}

// ---------------------------------------------------------------------------------------------
// findImplicitThinkingClose.

std::optional<ImplicitThinkingCloseBoundary> find_implicit_thinking_close(std::string_view content) {
    static const std::vector<std::string_view> kMarkers = {
        kCloseThink,
        kCloseThinking,
        kCloseReasoning,
        kCloseAnalysis,
        kCloseThought,
        "<|end|>",
        "<|channel|>final<|message|>",
        "<|start|>assistant<|channel|>final<|message|>",
        "<channel|>",
    };

    int inline_ticks = 0;
    char fence_character = 0;
    int fence_length = 0;
    bool line_prefix = true;
    int line_indent = 0;
    bool indented_code_line = false;
    std::size_t index = 0;

    const auto count_run = [&](char character) {
        std::size_t end = index;
        while (end < content.size() && content[end] == character) ++end;
        return end - index;
    };

    const auto update_line_state = [&](std::uint32_t code_point) {
        if (code_point == '\n') {
            line_prefix = true;
            line_indent = 0;
            indented_code_line = false;
        } else if (line_prefix && code_point == ' ' && line_indent < 4) {
            ++line_indent;
            if (line_indent == 4) indented_code_line = true;
        } else if (line_prefix && code_point == '\t') {
            indented_code_line = true;
            line_prefix = false;
        } else {
            line_prefix = false;
        }
    };

    const auto consume = [&](std::size_t length) {
        for (std::size_t offset = 0; offset < length; ++offset) {
            update_line_state(static_cast<unsigned char>(content[index + offset]));
        }
        index += length;
    };

    const auto consume_code_point = [&]() {
        const auto decoded = unicode::decode_utf8(content.data() + index, content.size() - index);
        update_line_state(decoded.code_point);
        index += decoded.length;
    };

    while (index < content.size()) {
        if (inline_ticks == 0 && fence_character == 0 && !indented_code_line) {
            for (const std::string_view marker : kMarkers) {
                if (detail::region_matches_ci(content, index, marker)) {
                    return ImplicitThinkingCloseBoundary{index, index + marker.size()};
                }
            }
        }

        const char character = content[index];
        if (fence_character != 0) {
            if (line_prefix && line_indent <= 3 && character == fence_character) {
                const std::size_t run = count_run(character);
                if (static_cast<int>(run) >= fence_length) {
                    fence_character = 0;
                    fence_length = 0;
                }
                consume(run);
            } else {
                consume_code_point();
            }
        } else if (inline_ticks > 0) {
            if (character == '`') {
                const std::size_t run = count_run(character);
                if (static_cast<int>(run) == inline_ticks) inline_ticks = 0;
                consume(run);
            } else {
                consume_code_point();
            }
        } else if (!indented_code_line && (character == '`' || character == '~')) {
            const std::size_t run = count_run(character);
            if (line_prefix && line_indent <= 3 && run >= 3) {
                fence_character = character;
                fence_length = static_cast<int>(run);
            } else if (character == '`') {
                inline_ticks = static_cast<int>(run);
            }
            consume(run);
        } else {
            consume_code_point();
        }
    }
    return std::nullopt;
}

}  // namespace agora::core::stream_normalization
