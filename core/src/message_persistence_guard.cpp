#include "agora/core/message_persistence_guard.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/unicode.hpp"

namespace agora::core::message_persistence_guard {
namespace {

using backup::MessageSegment;

constexpr std::size_t kTrimFloorUtf16 = 2000;

std::u16string to_utf16(std::string_view text) {
    std::u16string units;
    units.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unicode::Decoded decoded = unicode::decode_utf8(text.data() + offset, text.size() - offset);
        const std::uint32_t code_point = decoded.code_point;
        if (code_point <= 0xFFFF) {
            units.push_back(static_cast<char16_t>(code_point));
        } else {
            const std::uint32_t adjusted = code_point - 0x10000;
            units.push_back(static_cast<char16_t>(0xD800 + (adjusted >> 10)));
            units.push_back(static_cast<char16_t>(0xDC00 + (adjusted & 0x3FF)));
        }
        offset += decoded.length;
    }
    return units;
}

bool is_high_surrogate(char16_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
bool is_low_surrogate(char16_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

std::size_t text_units(std::string_view text) { return json::utf16_length(text); }

std::size_t optional_units(const std::optional<std::string>& text) {
    return text.has_value() ? text_units(*text) : 0;
}

/** UTF-8 bytes emitted for a string payload after kotlinx.serialization JSON escaping. */
std::int64_t json_string_payload_utf8_size(std::string_view value) {
    const std::u16string units = to_utf16(value);
    std::int64_t bytes = 0;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const char16_t unit = units[index];
        if (unit == u'"' || unit == u'\\') {
            bytes += 2;
        } else if (unit == u'\b' || unit == u'\t' || unit == u'\n' || unit == u'\f' ||
                   unit == u'\r') {
            bytes += 2;
        } else if (unit < 0x20) {
            bytes += 6;
        } else if (unit < 0x80) {
            bytes += 1;
        } else if (unit < 0x800) {
            bytes += 2;
        } else if (is_high_surrogate(unit) && index + 1 < units.size() &&
                   is_low_surrogate(units[index + 1])) {
            ++index;
            bytes += 4;
        } else if (is_high_surrogate(unit) || is_low_surrogate(unit)) {
            bytes += 1;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

/** UTF-8 size of the string itself, without JSON escaping. */
std::int64_t utf8_size(std::string_view value) {
    const std::u16string units = to_utf16(value);
    std::int64_t bytes = 0;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const char16_t unit = units[index];
        if (unit < 0x80) {
            bytes += 1;
        } else if (unit < 0x800) {
            bytes += 2;
        } else if (is_high_surrogate(unit) && index + 1 < units.size() &&
                   is_low_surrogate(units[index + 1])) {
            ++index;
            bytes += 4;
        } else if (is_high_surrogate(unit) || is_low_surrogate(unit)) {
            bytes += 1;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

std::size_t prefix_end_within_json_payload_bytes(std::string_view value, std::size_t max_end,
                                                 std::int64_t max_bytes) {
    const std::u16string units = to_utf16(value);
    std::int64_t bytes = 0;
    std::size_t index = 0;
    while (index < max_end) {
        const char16_t unit = units[index];
        std::size_t width = 1;
        std::int64_t char_bytes = 0;
        if (unit == u'"' || unit == u'\\') {
            char_bytes = 2;
        } else if (unit == u'\b' || unit == u'\t' || unit == u'\n' || unit == u'\f' ||
                   unit == u'\r') {
            char_bytes = 2;
        } else if (unit < 0x20) {
            char_bytes = 6;
        } else if (unit < 0x80) {
            char_bytes = 1;
        } else if (unit < 0x800) {
            char_bytes = 2;
        } else if (is_high_surrogate(unit) && index + 1 < units.size() &&
                   is_low_surrogate(units[index + 1])) {
            width = 2;
            char_bytes = 4;
        } else if (is_high_surrogate(unit) || is_low_surrogate(unit)) {
            char_bytes = 1;
        } else {
            char_bytes = 3;
        }
        if (index + width > max_end || bytes + char_bytes > max_bytes) break;
        bytes += char_bytes;
        index += width;
    }
    return index;
}

std::string halve_with_marker(std::string_view value) {
    const std::u16string units = to_utf16(value);
    if (units.size() <= kTrimFloorUtf16) return std::string(value);
    const std::size_t marker_units = text_units(kTruncationMarker);
    std::size_t preferred_end = std::max(units.size() / 2,
                                         kTrimFloorUtf16 - marker_units);
    preferred_end = std::min(preferred_end, units.size() - marker_units - 1);
    if (preferred_end >= 1 && preferred_end < units.size() &&
        is_high_surrogate(units[preferred_end - 1]) &&
        is_low_surrogate(units[preferred_end])) {
        --preferred_end;
    }

    const std::int64_t max_prefix_bytes = json_string_payload_utf8_size(value) -
                                          json_string_payload_utf8_size(kTruncationMarker) - 1;
    const std::size_t end =
        prefix_end_within_json_payload_bytes(value, preferred_end, max_prefix_bytes);
    return json::take_utf16_units(value, end) + std::string(kTruncationMarker);
}

std::int64_t trimmable_payload_bytes(const MessageSegment& segment) {
    std::int64_t bytes = 0;
    if (segment.tool_result) bytes += json_string_payload_utf8_size(*segment.tool_result);
    if (segment.tool_result_text) {
        bytes += json_string_payload_utf8_size(*segment.tool_result_text);
    }
    if (segment.tool_structured_result) {
        bytes += json_string_payload_utf8_size(*segment.tool_structured_result);
    }
    if (segment.tool_progress) bytes += json_string_payload_utf8_size(*segment.tool_progress);
    if (segment.type != "tool") bytes += json_string_payload_utf8_size(segment.content);
    return bytes;
}

std::int64_t trimmable_payload_bytes(const std::vector<MessageSegment>& segments) {
    std::int64_t bytes = 0;
    for (const MessageSegment& segment : segments) bytes += trimmable_payload_bytes(segment);
    return bytes;
}

/** Size of the field that trimming would shrink, driving largest-first selection. */
std::size_t trimmable_size(const MessageSegment& segment) {
    const std::size_t result = optional_units(segment.tool_result);
    const std::size_t result_text = optional_units(segment.tool_result_text);
    const std::size_t structured = optional_units(segment.tool_structured_result);
    const std::size_t progress = optional_units(segment.tool_progress);
    const std::size_t content = segment.type == "tool" ? 0 : text_units(segment.content);
    return std::max({result, result_text, structured, progress, content});
}

bool can_trim(const MessageSegment& segment) {
    return (segment.tool_result && text_units(*segment.tool_result) > kTrimFloorUtf16) ||
           (segment.tool_result_text && text_units(*segment.tool_result_text) > kTrimFloorUtf16) ||
           (segment.tool_structured_result &&
            text_units(*segment.tool_structured_result) > kTrimFloorUtf16) ||
           (segment.tool_progress && text_units(*segment.tool_progress) > kTrimFloorUtf16) ||
           (segment.type != "tool" && text_units(segment.content) > kTrimFloorUtf16);
}

/** Halve the largest trimmable field, preferring the tool result on ties. */
MessageSegment trim_largest(const MessageSegment& segment) {
    const std::size_t result = optional_units(segment.tool_result);
    const std::size_t result_text = optional_units(segment.tool_result_text);
    const std::size_t structured = optional_units(segment.tool_structured_result);
    const std::size_t progress = optional_units(segment.tool_progress);
    const std::size_t content = segment.type == "tool" ? 0 : text_units(segment.content);
    const std::size_t largest = std::max({result, result_text, structured, progress, content});

    if (segment.tool_result && result == largest && result > kTrimFloorUtf16) {
        MessageSegment trimmed = segment;
        trimmed.tool_result = halve_with_marker(*segment.tool_result);
        return trimmed;
    }
    if (segment.tool_result_text && result_text == largest &&
        result_text > kTrimFloorUtf16) {
        MessageSegment trimmed = segment;
        trimmed.tool_result_text = halve_with_marker(*segment.tool_result_text);
        return trimmed;
    }
    if (segment.tool_structured_result && structured == largest &&
        structured > kTrimFloorUtf16) {
        MessageSegment trimmed = segment;
        trimmed.tool_structured_result = halve_with_marker(*segment.tool_structured_result);
        return trimmed;
    }
    if (segment.tool_progress && progress == largest && progress > kTrimFloorUtf16) {
        MessageSegment trimmed = segment;
        trimmed.tool_progress = halve_with_marker(*segment.tool_progress);
        return trimmed;
    }
    if (segment.type != "tool" && text_units(segment.content) > kTrimFloorUtf16) {
        MessageSegment trimmed = segment;
        trimmed.content = halve_with_marker(segment.content);
        return trimmed;
    }
    return segment;
}

std::optional<std::string> oversized_result(const std::vector<MessageSegment>& segments,
                                            int max_bytes) {
    for (const MessageSegment& segment : segments) {
        if (!segment.response_output_items.empty()) {
            throw PersistenceBudgetError("Responses continuation state exceeds the " +
                                         std::to_string(max_bytes) +
                                         "-byte persistence budget");
        }
    }
    return std::nullopt;
}

}  // namespace

std::string clip_text(std::string_view text) {
    const std::u16string units = to_utf16(text);
    if (units.size() <= static_cast<std::size_t>(kMaxPersistedTextChars)) {
        return std::string(text);
    }
    std::size_t end = static_cast<std::size_t>(kMaxPersistedTextChars);
    if (end >= 1 && end < units.size() && is_high_surrogate(units[end - 1]) &&
        is_low_surrogate(units[end])) {
        --end;
    }
    return json::take_utf16_units(text, end) + std::string(kTruncationMarker);
}

std::optional<std::string> encode_segments_bounded(
    const std::vector<MessageSegment>& segments, int max_bytes) {
    if (segments.empty()) return std::nullopt;
    const std::int64_t budget = max_bytes;
    const std::int64_t original_payload_bytes = trimmable_payload_bytes(segments);
    std::optional<std::string> original_oversized_json;

    // A payload that fits by itself may still fit with metadata. Preserve byte-for-byte output
    // for that ordinary path by using the canonical encoder directly.
    if (original_payload_bytes <= budget) {
        std::string json = backup::encode_message_segments(segments);
        if (utf8_size(json) <= budget) return json;
        original_oversized_json = std::move(json);
    }

    bool any_trimmable = false;
    for (const MessageSegment& segment : segments) {
        if (can_trim(segment)) {
            any_trimmable = true;
            break;
        }
    }
    if (!any_trimmable) return oversized_result(segments, max_bytes);

    std::vector<MessageSegment> placeholder_segments;
    placeholder_segments.reserve(segments.size());
    std::int64_t placeholder_count = 0;
    for (const MessageSegment& segment : segments) {
        MessageSegment projected = segment;
        if (!(projected.type == "tool" || projected.content.empty())) {
            ++placeholder_count;
            projected.content = "x";
        }
        auto replace = [&](std::optional<std::string>& value) {
            if (!value.has_value() || value->empty()) return;
            ++placeholder_count;
            value = "x";
        };
        replace(projected.tool_result);
        replace(projected.tool_progress);
        replace(projected.tool_result_text);
        replace(projected.tool_structured_result);
        placeholder_segments.push_back(std::move(projected));
    }
    const std::int64_t fixed_bytes =
        utf8_size(backup::encode_message_segments(placeholder_segments)) - placeholder_count;

    std::vector<MessageSegment> current = segments;
    bool changed = false;
    std::int64_t projected_bytes = fixed_bytes + original_payload_bytes;
    while (projected_bytes > budget) {
        std::size_t pick_index = 0;
        std::size_t pick_size = 0;
        for (std::size_t index = 0; index < current.size(); ++index) {
            const std::size_t size = trimmable_size(current[index]);
            if (index == 0 || size > pick_size) {
                pick_index = index;
                pick_size = size;
            }
        }
        const MessageSegment& segment = current[pick_index];
        if (!can_trim(segment)) return oversized_result(current, max_bytes);

        const MessageSegment trimmed = trim_largest(segment);
        const std::int64_t old_payload = trimmable_payload_bytes(segment);
        const std::int64_t new_payload = trimmable_payload_bytes(trimmed);
        if (new_payload >= old_payload) return oversized_result(current, max_bytes);

        current[pick_index] = trimmed;
        changed = true;
        projected_bytes -= old_payload - new_payload;
    }

    std::string json;
    if (!changed && original_oversized_json.has_value()) {
        json = *original_oversized_json;
    } else {
        json = backup::encode_message_segments(current);
    }
    if (utf8_size(json) <= budget) return json;

    // The estimator mirrors kotlinx.serialization JSON escaping. Keep the exact encoded-byte
    // check authoritative so a future serializer behavior change cannot persist an oversized row
    // or silently discard protected continuation state.
    return oversized_result(current, max_bytes);
}

}  // namespace agora::core::message_persistence_guard
