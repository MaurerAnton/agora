#include "agora/core/shell_tools.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::shell_tools {
namespace {

namespace json = agora::core::json;

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

// ---------------------------------------------------------------------------------------------
// UTF-16 machinery for the bounded slice (Kotlin strings index UTF-16 code units).
// ---------------------------------------------------------------------------------------------

struct Utf16Text {
    std::vector<char32_t> points;          // decoded code points
    std::vector<std::size_t> prefix_units; // units before point k (size points.size() + 1)
    std::size_t units = 0;
};

Utf16Text to_utf16(std::string_view text) {
    Utf16Text result;
    result.prefix_units.push_back(0);
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F;
            extra = 2;
        } else {
            cp = lead & 0x07;
            extra = 3;
        }
        ++i;
        for (std::size_t k = 0; k < extra && i < text.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        }
        result.points.push_back(cp);
        result.units += cp > 0xFFFF ? 2 : 1;
        result.prefix_units.push_back(result.units);
    }
    return result;
}

/** The UTF-8 bytes of the first [count] code points. */
std::string prefix_utf8(const std::vector<char32_t>& points, std::size_t count) {
    std::string out;
    for (std::size_t k = 0; k < count && k < points.size(); ++k) {
        const char32_t cp = points[k];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

/** The code point count whose UTF-16 prefix is exactly [unit_index] units (or the closest
 *  boundary not past it, for inputs the surrogate guard leaves alone). */
std::size_t points_for_units(const Utf16Text& text, std::size_t unit_index) {
    std::size_t low = 0;
    std::size_t high = text.points.size();
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        if (text.prefix_units[mid] <= unit_index) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

bool is_high_surrogate(std::uint16_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
bool is_low_surrogate(std::uint16_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

/** The UTF-16 code unit at [index] (valid surrogate pairs guaranteed by the guard). */
std::uint16_t unit_at(const Utf16Text& text, std::size_t index) {
    // Walk the prefix table to the point containing this unit.
    std::size_t low = 0;
    std::size_t high = text.points.size();
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        if (text.prefix_units[mid] <= index) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    const char32_t cp = text.points[low];
    const std::size_t start = text.prefix_units[low];
    if (cp > 0xFFFF) {
        const char32_t v = cp - 0x10000;
        return index == start ? static_cast<std::uint16_t>(0xD800 + (v >> 10))
                              : static_cast<std::uint16_t>(0xDC00 + (v & 0x3FF));
    }
    return static_cast<std::uint16_t>(cp);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ShellToolJson.kt
// ---------------------------------------------------------------------------------------------

std::vector<std::pair<std::string, json::Value>> parse_tool_args(std::string_view arguments) {
    const std::string text = json::is_blank(arguments) ? std::string("{}") : std::string(arguments);
    const json::ParseResult parsed = json::parse(text);
    std::vector<std::pair<std::string, json::Value>> args;
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return args;
    for (const json::Member& entry : parsed.value->members) {
        bool replaced = false;
        for (auto& [key, value] : args) {
            if (key == entry.key) {
                value = entry.value;  // LinkedHashMap.put: last wins, first position kept
                replaced = true;
                break;
            }
        }
        if (!replaced) args.emplace_back(entry.key, entry.value);
    }
    return args;
}

std::string arg(const std::vector<std::pair<std::string, json::Value>>& args,
                std::string_view key) {
    for (const auto& [entry_key, value] : args) {
        if (entry_key != key) continue;
        if (value.kind == json::Kind::String || value.kind == json::Kind::Literal) {
            return value.text;  // JsonNull is a primitive: its content is the text "null"
        }
        return std::string();
    }
    return std::string();
}

bool bool_arg(const std::vector<std::pair<std::string, json::Value>>& args,
              std::string_view key) {
    for (const auto& [entry_key, value] : args) {
        if (entry_key != key) continue;
        if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) return false;
        return value.text == "true";
    }
    return false;
}

std::string json_error(std::string_view type, std::string_view message,
                       const std::optional<std::string>& server,
                       const std::optional<std::string>& command) {
    json::Value object;
    object.kind = json::Kind::Object;
    if (!json::is_blank(type)) {
        object.members.push_back({"type", json_string(std::string(type))});
    }
    object.members.push_back({"error", json_string("error")});
    object.members.push_back({"message", json_string(std::string(message))});
    if (server) object.members.push_back({"server", json_string(*server)});
    if (command) object.members.push_back({"command", json_string(*command)});
    return json::to_kotlin_string(object);
}

// ---------------------------------------------------------------------------------------------
// BoundedFileReadJson.kt
// ---------------------------------------------------------------------------------------------

std::int32_t shell_file_line_count(std::string_view content) {
    if (content.empty()) return 0;
    std::int32_t count = 1;
    for (char c : content) {
        if (c == '\n') ++count;
    }
    return count;
}

std::string bounded_file_read_json(const std::string& server, const std::string& path,
                                   const ShellFileReadResult& result,
                                   std::int32_t max_length) {
    if (max_length <= 0) {
        throw openai::ProviderRequestError{"IllegalArgumentException", "Failed requirement."};
    }

    const auto encode = [&](const std::string& content, bool truncated) {
        json::Value object;
        object.kind = json::Kind::Object;
        object.members.push_back({"type", json_string("file_read")});
        object.members.push_back({"server", json_string(server)});
        object.members.push_back({"path", json_string(path)});
        object.members.push_back({"content", json_string(content)});
        object.members.push_back({"lines", json_literal(std::to_string(shell_file_line_count(content)))});
        object.members.push_back({"total_lines", json_literal(std::to_string(result.total_lines))});
        object.members.push_back({"total_bytes", json_literal(std::to_string(result.total_bytes))});
        object.members.push_back(
            {"returned_bytes", json_literal(std::to_string(content.size()))});
        object.members.push_back({"offset", json_literal(std::to_string(result.offset))});
        object.members.push_back({"limit", json_literal(std::to_string(result.limit))});
        object.members.push_back({"truncated", json_literal(truncated ? "true" : "false")});
        return json::to_kotlin_string(object);
    };

    const std::string full = encode(result.content, result.truncated);
    if (static_cast<std::int64_t>(json::utf16_length(full)) <= max_length) return full;

    const Utf16Text text = to_utf16(result.content);
    std::string best = encode(std::string(), true);
    std::int64_t low = 0;
    std::int64_t high = static_cast<std::int64_t>(text.units);
    while (low <= high) {
        const std::int64_t midpoint = low + (high - low) / 2;
        std::size_t safe_length = static_cast<std::size_t>(midpoint);
        if (midpoint >= 1 && midpoint < static_cast<std::int64_t>(text.units) &&
            is_high_surrogate(unit_at(text, safe_length - 1)) &&
            is_low_surrogate(unit_at(text, safe_length))) {
            safe_length -= 1;
        }
        const std::string candidate =
            encode(prefix_utf8(text.points, points_for_units(text, safe_length)), true);
        if (static_cast<std::int64_t>(json::utf16_length(candidate)) <= max_length) {
            best = candidate;
            low = midpoint + 1;
        } else {
            high = midpoint - 1;
        }
    }
    return best;
}

}  // namespace agora::core::shell_tools
