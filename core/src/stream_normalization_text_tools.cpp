// StreamingTextToolCallParser and ToolCallTextParser.parse.
//
// Protocol literals are split with adjacent string-literal concatenation exactly like the Kotlin
// sources split them, so no bare tag literal is a contiguous substring of this file on disk.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/stream_normalization.hpp"
#include "stream_normalization_internal.hpp"

namespace agora::core::stream_normalization {

namespace json = agora::core::json;

namespace {

// Composed at compile time from pieces; see the file header.
constexpr std::string_view kOpenTag = "<" "tool_" "call>";
constexpr std::string_view kCloseTag = "<" "/tool_" "call>";
constexpr std::string_view kInvokeOpen = "<" "invoke";
constexpr std::string_view kInvokeClose = "<" "/invoke";
constexpr std::string_view kAntmlInvokeOpen = "<" "antml:" "invoke";
constexpr std::string_view kAntmlInvokeClose = "<" "/antml:" "invoke";
constexpr std::string_view kParameter = "parameter";

const std::vector<std::string_view>& bare_json_prefixes() {
    static const std::vector<std::string_view> kPrefixes = {
        "{\"name\"",
        "{\"function\"",
        "[{\"name\"",
        "[{\"function\"",
    };
    return kPrefixes;
}

const std::string& required_stream_key(const std::optional<std::string>& value) {
    if (!value.has_value()) throw IllegalStateError("stream key missing");
    return *value;
}

/** Kotlin String.replace(oldValue, newValue): non-overlapping, left to right. */
std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        if (text.size() - index >= from.size() && text.compare(index, from.size(), from) == 0) {
            out += to;
            index += from.size();
        } else {
            out.push_back(text[index]);
            ++index;
        }
    }
    return out;
}

/** Kotlin String.replace(oldValue, newValue, ignoreCase = true). */
std::string replace_all_ci(std::string_view text, std::string_view from, std::string_view to) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        if (detail::region_matches_ci(text, index, from)) {
            out += to;
            index += from.size();
        } else {
            out.push_back(text[index]);
            ++index;
        }
    }
    return out;
}

/** Java Pattern line terminators: \n, \r, \u0085, \u2028, \u2029 (dot's exclusions). */
std::size_t line_terminator_length(std::string_view text, std::size_t index) {
    if (index >= text.size()) return 0;
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead == '\n' || lead == '\r') return 1;
    if (lead == 0xC2 && index + 1 < text.size() &&
        static_cast<unsigned char>(text[index + 1]) == 0x85) {
        return 2;
    }
    if (lead == 0xE2 && index + 2 < text.size() &&
        static_cast<unsigned char>(text[index + 1]) == 0x80) {
        const unsigned char third = static_cast<unsigned char>(text[index + 2]);
        if (third == 0xA8 || third == 0xA9) return 3;
    }
    return 0;
}

/**
 * NAME_FIELD = Regex(""""name"\s*:\s*"((?:\\.|[^"\\])*)"""") — the raw (still escaped)
 * capture of the first match. `\\.` is backslash plus one non-line-terminator character, so a
 * backslash followed by a line terminator makes the whole match at that position fail.
 */
std::optional<std::string> name_field_capture(std::string_view body) {
    static constexpr std::string_view kNameKey = "\"name\"";
    if (body.size() < kNameKey.size()) return std::nullopt;
    for (std::size_t start = 0; start + kNameKey.size() <= body.size(); ++start) {
        if (body.compare(start, kNameKey.size(), kNameKey) != 0) continue;
        std::size_t cursor = start + kNameKey.size();
        while (cursor < body.size() && detail::is_java_space(body[cursor])) ++cursor;
        if (cursor >= body.size() || body[cursor] != ':') continue;
        ++cursor;
        while (cursor < body.size() && detail::is_java_space(body[cursor])) ++cursor;
        if (cursor >= body.size() || body[cursor] != '"') continue;
        ++cursor;
        std::string captured;
        bool closed = false;
        bool viable = true;
        while (cursor < body.size()) {
            const char c = body[cursor];
            if (c == '\\') {
                if (cursor + 1 >= body.size()) {
                    viable = false;
                    break;
                }
                if (line_terminator_length(body, cursor + 1) != 0) {
                    viable = false;
                    break;
                }
                const std::size_t step =
                    1 + detail::utf8_sequence_length(body.data() + cursor + 1, body.size() - cursor - 1);
                captured.append(body.substr(cursor, step));
                cursor += step;
                continue;
            }
            if (c == '"') {
                closed = true;
                break;
            }
            captured.push_back(c);
            ++cursor;
        }
        if (viable && closed) return captured;
    }
    return std::nullopt;
}

/**
 * ARGUMENTS_KEY = Regex("""(?:arguments|parameters)"\s*:\s*"""): index just past the first
 * complete match, i.e. right after any whitespace that follows the colon.
 */
std::optional<std::size_t> arguments_key_start(std::string_view body) {
    static constexpr std::string_view kArguments = "\"arguments\"";
    static constexpr std::string_view kParameters = "\"parameters\"";
    for (std::size_t start = 0; start < body.size(); ++start) {
        std::size_t cursor = 0;
        if (start + kArguments.size() <= body.size() &&
            body.compare(start, kArguments.size(), kArguments) == 0) {
            cursor = start + kArguments.size();
        } else if (start + kParameters.size() <= body.size() &&
                   body.compare(start, kParameters.size(), kParameters) == 0) {
            cursor = start + kParameters.size();
        } else {
            continue;
        }
        while (cursor < body.size() && detail::is_java_space(body[cursor])) ++cursor;
        if (cursor >= body.size() || body[cursor] != ':') continue;
        ++cursor;
        while (cursor < body.size() && detail::is_java_space(body[cursor])) ++cursor;
        return cursor;
    }
    return std::nullopt;
}

std::string partial_arguments(std::string_view source) {
    std::string trimmed = detail::trim_start(source);
    if (trimmed.empty() || trimmed.front() != '"') return trimmed;
    trimmed.erase(0, 1);  // removePrefix("\"")
    if (!trimmed.empty() && trimmed.back() == '"') trimmed.pop_back();  // removeSuffix("\"")
    trimmed = replace_all(trimmed, "\\\"", "\"");
    trimmed = replace_all(trimmed, "\\\\", "\\");
    return trimmed;
}

std::string decode_xml(std::string_view value) {
    std::string out(value);
    out = replace_all_ci(out, "&lt;", "<");
    out = replace_all_ci(out, "&gt;", ">");
    out = replace_all_ci(out, "&quot;", "\"");
    out = replace_all_ci(out, "&apos;", "'");
    out = replace_all_ci(out, "&amp;", "&");
    return out;
}

// -- XML invoke scanning ------------------------------------------------------------------------
//
// Mirrors: <(?:(?:antml):)?KEYWORD\s+name\s*=\s*["']([^"']+)["'][^>]*>([\s\S]*?)</(?:(?:antml):)?KEYWORD\s*>
// with RegexOption.IGNORE_CASE and findAll's leftmost, non-overlapping semantics.

struct XmlTagBlock {
    std::string group_name;  // the name="..." attribute value (raw)
    std::string body;        // lazy body between the closing '>' and the closing tag
    std::string full;        // the entire matched text
};

/** True at positions where java.util.regex `\b` matches for JVM word characters. */
bool word_boundary_at(std::string_view text, std::size_t index) {
    const auto is_word = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_';
    };
    const bool before = index > 0 && is_word(text[index - 1]);
    const bool at = index < text.size() && is_word(text[index]);
    return before != at;
}

std::vector<XmlTagBlock> find_xml_tag_blocks(std::string_view content, std::string_view keyword) {
    std::vector<XmlTagBlock> matches;
    std::size_t search_from = 0;
    while (search_from < content.size()) {
        bool found = false;
        std::size_t pos = search_from;
        while (pos < content.size()) {
            std::size_t advance = pos + 1;
            if (content[pos] != '<') {
                pos = advance;
                continue;
            }
            std::size_t cursor = pos + 1;
            if (detail::region_matches_ci(content, cursor, "antml:")) cursor += 6;
            if (!detail::region_matches_ci(content, cursor, keyword)) {
                pos = advance;
                continue;
            }
            cursor += keyword.size();
            const std::size_t space_start = cursor;
            while (cursor < content.size() && detail::is_java_space(content[cursor])) ++cursor;
            if (cursor == space_start) {  // \s+ needs at least one
                pos = advance;
                continue;
            }
            if (!detail::region_matches_ci(content, cursor, "name")) {
                pos = advance;
                continue;
            }
            cursor += 4;
            while (cursor < content.size() && detail::is_java_space(content[cursor])) ++cursor;
            if (cursor >= content.size() || content[cursor] != '=') {
                pos = advance;
                continue;
            }
            ++cursor;
            while (cursor < content.size() && detail::is_java_space(content[cursor])) ++cursor;
            if (cursor >= content.size() || (content[cursor] != '"' && content[cursor] != '\'')) {
                pos = advance;
                continue;
            }
            const std::size_t name_start = cursor + 1;
            std::size_t name_end = name_start;
            while (name_end < content.size() && content[name_end] != '"' && content[name_end] != '\'') {
                ++name_end;
            }
            if (name_end == name_start || name_end >= content.size()) {
                pos = advance;
                continue;
            }
            cursor = name_end + 1;  // past the closing quote (either quote kind, like the regex)
            while (cursor < content.size() && content[cursor] != '>') ++cursor;
            if (cursor >= content.size()) {
                pos = advance;
                continue;
            }
            ++cursor;  // past '>'
            std::size_t body_end = cursor;
            bool matched_body = false;
            std::size_t match_end = 0;
            while (body_end <= content.size()) {
                if (body_end + 2 <= content.size() && content[body_end] == '<' &&
                    content[body_end + 1] == '/') {
                    std::size_t close_cursor = body_end + 2;
                    if (detail::region_matches_ci(content, close_cursor, "antml:")) close_cursor += 6;
                    if (detail::region_matches_ci(content, close_cursor, keyword)) {
                        close_cursor += keyword.size();
                        while (close_cursor < content.size() &&
                               detail::is_java_space(content[close_cursor])) {
                            ++close_cursor;
                        }
                        if (close_cursor < content.size() && content[close_cursor] == '>') {
                            matched_body = true;
                            match_end = close_cursor + 1;
                            break;
                        }
                    }
                }
                ++body_end;
            }
            if (!matched_body) {
                pos = advance;
                continue;
            }
            XmlTagBlock block;
            block.group_name = std::string(content.substr(name_start, name_end - name_start));
            block.body = std::string(content.substr(cursor, body_end - cursor));
            block.full = std::string(content.substr(pos, match_end - pos));
            matches.push_back(std::move(block));
            pos = match_end;
            found = true;
            break;
        }
        if (!found) break;
        search_from = pos;
    }
    return matches;
}

/** XML_ID_ATTRIBUTE_NAME = Regex("""\b(?:id|call_id)\s*=""", IGNORE_CASE). */
bool xml_id_attribute_name_matches(std::string_view text) {
    for (std::size_t start = 0; start < text.size(); ++start) {
        if (!word_boundary_at(text, start)) continue;
        for (const std::string_view candidate : {"id", "call_id"}) {
            if (!detail::region_matches_ci(text, start, candidate)) continue;
            std::size_t cursor = start + candidate.size();
            while (cursor < text.size() && detail::is_java_space(text[cursor])) ++cursor;
            if (cursor < text.size() && text[cursor] == '=') return true;
        }
    }
    return false;
}

/** XML_ID_ATTRIBUTE = Regex("""\b(?:id|call_id)\s*=\s*(["'])([^"']+)\1""", IGNORE_CASE). */
std::optional<std::string> xml_id_attribute_find(std::string_view text) {
    for (std::size_t start = 0; start < text.size(); ++start) {
        if (!word_boundary_at(text, start)) continue;
        for (const std::string_view candidate : {"id", "call_id"}) {
            if (!detail::region_matches_ci(text, start, candidate)) continue;
            std::size_t cursor = start + candidate.size();
            while (cursor < text.size() && detail::is_java_space(text[cursor])) ++cursor;
            if (cursor >= text.size() || text[cursor] != '=') continue;
            ++cursor;
            while (cursor < text.size() && detail::is_java_space(text[cursor])) ++cursor;
            if (cursor >= text.size() || (text[cursor] != '"' && text[cursor] != '\'')) continue;
            const char quote = text[cursor];
            ++cursor;
            const std::size_t value_start = cursor;
            while (cursor < text.size() && text[cursor] != '"' && text[cursor] != '\'') ++cursor;
            if (cursor == value_start) continue;
            if (cursor >= text.size() || text[cursor] != quote) continue;  // the \1 backreference
            return std::string(text.substr(value_start, cursor - value_start));
        }
    }
    return std::nullopt;
}

// -- ToolCallTextParser.parse -------------------------------------------------------------------

const json::Value* member_or_null(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const auto& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** stringField: (obj[key] as? JsonPrimitive)?.let { if (it.isString) it.content else null }. */
std::optional<std::string> string_field(const json::Value& object, std::string_view key) {
    const json::Value* value = member_or_null(object, key);
    if (value == nullptr || value->kind != json::Kind::String) return std::nullopt;
    return value->text;
}

/** normalizeArguments: the value must re-parse to one complete JSON object. */
std::optional<std::string> normalize_arguments(const json::Value& element) {
    const std::string raw = element.kind == json::Kind::String
        ? element.text
        : json::to_kotlin_string(element);
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return std::nullopt;
    return json::to_kotlin_string(*parsed.value);
}

std::optional<ParsedTextCall> parse_call_json(std::string_view json_str) {
    const json::ParseResult parsed = json::parse(json_str);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return std::nullopt;
    const json::Value& object = *parsed.value;

    const json::Value* function = member_or_null(object, "function");
    const json::Value* function_object =
        (function != nullptr && function->kind == json::Kind::Object) ? function : nullptr;

    std::optional<std::string> name = string_field(object, "name");
    if (!name.has_value() && function_object != nullptr) {
        name = string_field(*function_object, "name");
    }
    if (!name.has_value() || !detail::is_safe_wire_tool_name(*name)) return std::nullopt;

    std::vector<std::string> ids;
    for (const std::string_view key : {"id", "call_id"}) {
        if (member_or_null(object, key) == nullptr) continue;  // containsKey, even for null
        std::optional<std::string> value = string_field(object, key);
        if (!value.has_value()) return std::nullopt;
        ids.push_back(*value);
    }
    std::vector<std::string> distinct;
    for (const auto& id : ids) {
        bool seen = false;
        for (const auto& other : distinct) {
            if (other == id) {
                seen = true;
                break;
            }
        }
        if (!seen) distinct.push_back(id);
    }
    if (distinct.size() > 1) return std::nullopt;
    const std::optional<std::string> id =
        distinct.empty() ? std::nullopt : std::optional<std::string>(distinct.front());
    if (id.has_value() && !detail::is_safe_wire_tool_call_id(*id)) return std::nullopt;

    const json::Value* args = member_or_null(object, "arguments");
    if (args == nullptr) args = member_or_null(object, "parameters");
    if (args == nullptr && function_object != nullptr) {
        args = member_or_null(*function_object, "arguments");
    }
    if (args == nullptr && function_object != nullptr) {
        args = member_or_null(*function_object, "parameters");
    }
    std::string arguments = "{}";
    if (args != nullptr) {
        std::optional<std::string> normalized = normalize_arguments(*args);
        if (!normalized.has_value()) return std::nullopt;
        arguments = std::move(*normalized);
    }
    return ParsedTextCall{id, *name, std::move(arguments)};
}

void set_linked(std::vector<std::pair<std::string, std::string>>& entries,
                std::string key,
                std::string value) {
    for (auto& entry : entries) {
        if (entry.first == key) {
            entry.second = std::move(value);  // duplicate key replaces, keeps first position
            return;
        }
    }
    entries.emplace_back(std::move(key), std::move(value));
}

std::optional<ParsedTextCall> parse_xml_invoke(const XmlTagBlock& block) {
    const std::string name = json::trim(decode_xml(block.group_name));
    if (!detail::is_safe_wire_tool_name(name)) return std::nullopt;

    const std::size_t opening_end = block.full.find('>');
    const std::string opening_tag =
        opening_end == std::string::npos ? block.full : block.full.substr(0, opening_end);
    const bool id_attribute_present = xml_id_attribute_name_matches(opening_tag);
    std::optional<std::string> id;
    if (const auto raw_id = xml_id_attribute_find(opening_tag)) {
        id = json::trim(decode_xml(*raw_id));
    }
    if (id_attribute_present && (!id.has_value() || !detail::is_safe_wire_tool_call_id(*id))) {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& parameter : find_xml_tag_blocks(block.body, kParameter)) {
        const std::string key = json::trim(decode_xml(parameter.group_name));
        if (json::is_blank(key)) continue;
        const std::string value = json::trim(decode_xml(parameter.body));
        set_linked(entries, key, value);
    }

    json::Value object;
    object.kind = json::Kind::Object;
    object.members.reserve(entries.size());
    for (auto& entry : entries) {
        json::Value value;
        value.kind = json::Kind::String;
        value.text = std::move(entry.second);
        object.members.push_back(json::Member{std::move(entry.first), std::move(value)});
    }
    return ParsedTextCall{std::move(id), name, json::to_kotlin_string(object)};
}

}  // namespace

std::vector<ParsedTextCall> parse_tool_call_text(std::string_view content) {
    std::vector<ParsedTextCall> results;
    std::size_t idx = 0;
    bool saw_tagged_block = false;
    bool malformed_tagged_block = false;
    while (true) {
        const std::size_t start = content.find(kOpenTag, idx);
        if (start == std::string_view::npos) break;
        saw_tagged_block = true;
        const std::size_t inner_start = start + kOpenTag.size();
        const std::size_t end = content.find(kCloseTag, inner_start);
        if (end == std::string_view::npos) {
            malformed_tagged_block = true;
            break;
        }
        const std::string inner = json::trim(content.substr(inner_start, end - inner_start));
        std::optional<ParsedTextCall> parsed = parse_call_json(inner);
        if (!parsed.has_value()) {
            malformed_tagged_block = true;
        } else {
            results.push_back(std::move(*parsed));
        }
        idx = end + kCloseTag.size();
    }
    if (saw_tagged_block) {
        return malformed_tagged_block ? std::vector<ParsedTextCall>{} : results;
    }

    // Anthropic relays and prompt-based shims serialize native tool_use blocks as XML text.
    for (const XmlTagBlock& block : find_xml_tag_blocks(content, kInvokeOpen.substr(1))) {
        if (std::optional<ParsedTextCall> parsed = parse_xml_invoke(block)) {
            results.push_back(std::move(*parsed));
        }
    }
    if (!results.empty()) return results;

    const std::string trimmed = json::trim(content);
    if (trimmed.empty() || (trimmed.front() != '{' && trimmed.front() != '[')) {
        return std::vector<ParsedTextCall>{};
    }
    if (std::optional<ParsedTextCall> single = parse_call_json(trimmed)) {
        results.push_back(std::move(*single));
        return results;
    }
    if (trimmed.front() == '[') {
        const json::ParseResult parsed = json::parse(trimmed);
        if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Array) {
            return std::vector<ParsedTextCall>{};
        }
        std::vector<ParsedTextCall> array_calls;
        for (const json::Value& element : parsed.value->elements) {
            if (element.kind != json::Kind::Object) return std::vector<ParsedTextCall>{};
            std::optional<ParsedTextCall> call = parse_call_json(json::to_kotlin_string(element));
            if (!call.has_value()) return std::vector<ParsedTextCall>{};
            array_calls.push_back(std::move(*call));
        }
        return array_calls;
    }
    return std::vector<ParsedTextCall>{};
}

// ---------------------------------------------------------------------------------------------
// StreamingTextToolCallParser.

StreamingTextToolCallParser::StreamingTextToolCallParser(UuidFn uuid) : uuid_(std::move(uuid)) {}

std::string StreamingTextToolCallParser::new_stream_key() const {
    return "text_tool_" + (uuid_ ? uuid_() : random_uuid_v4());
}

void StreamingTextToolCallParser::feed(const std::string& content, const Callbacks& callbacks) {
    if (content.empty()) return;
    buffer_ += content;
    drain(callbacks);
}

void StreamingTextToolCallParser::flush(const Callbacks& callbacks) {
    switch (mode_) {
        case Mode::kNormal:
            if (!buffer_.empty()) callbacks.on_text(buffer_);
            break;
        case Mode::kTaggedCall:
            announce_partial(buffer_, callbacks);
            callbacks.on_malformed("Provider ended before the tagged tool call was complete");
            break;
        case Mode::kBareJsonCall:
            complete_bare_json(callbacks);
            break;
    }
    reset_after_call();
    buffer_.clear();
    can_probe_bare_json_ = false;
}

void StreamingTextToolCallParser::drain(const Callbacks& callbacks) {
    while (true) {
        if (mode_ == Mode::kNormal) {
            if (can_probe_bare_json_) {
                const std::string candidate = detail::trim_start(buffer_);
                if (candidate.empty()) return;
                bool matches_bare = false;
                for (const std::string_view prefix : bare_json_prefixes()) {
                    if (candidate.compare(0, prefix.size(), prefix) == 0) {
                        matches_bare = true;
                        break;
                    }
                }
                if (matches_bare) {
                    begin_call(Mode::kBareJsonCall, callbacks);
                    announce_partial(buffer_, callbacks);
                    return;
                }
                bool is_prefix_of_candidate = false;
                for (const std::string_view prefix : bare_json_prefixes()) {
                    if (candidate.size() <= prefix.size() &&
                        prefix.compare(0, candidate.size(), candidate) == 0) {
                        is_prefix_of_candidate = true;
                        break;
                    }
                }
                if (is_prefix_of_candidate) return;
                can_probe_bare_json_ = false;
            }

            const std::optional<std::pair<std::size_t, TaggedProtocol>> tagged_start =
                find_tagged_start();
            if (tagged_start.has_value()) {
                const std::size_t open_at = tagged_start->first;
                const TaggedProtocol protocol = tagged_start->second;
                if (open_at > 0) callbacks.on_text(buffer_.substr(0, open_at));
                std::size_t open_end = 0;
                if (protocol == TaggedProtocol::kJson) {
                    open_end = open_at + kOpenTag.size();
                } else {
                    // XML invoke carries the tool name in the opening tag, so the full opening
                    // tag is retained in the buffer while still being withheld from the UI.
                    const std::size_t close_bracket = buffer_.find('>', open_at);
                    if (close_bracket == std::string::npos) return;
                    open_end = close_bracket + 1;
                }
                const std::string opening = buffer_.substr(open_at, open_end - open_at);
                buffer_.erase(0, open_end);
                active_tagged_protocol_ = protocol;
                begin_call(Mode::kTaggedCall, callbacks);
                if (protocol == TaggedProtocol::kXml) buffer_.insert(0, opening);
                continue;
            }

            const std::size_t retained = longest_tag_prefix_suffix();
            const std::size_t safe_length = buffer_.size() - retained;
            if (safe_length > 0) {
                callbacks.on_text(buffer_.substr(0, safe_length));
                buffer_.erase(0, safe_length);
            }
            return;
        }

        if (mode_ == Mode::kTaggedCall) {
            std::optional<std::string> close_tag;
            if (active_tagged_protocol_ == TaggedProtocol::kJson) {
                close_tag = std::string(kCloseTag);
            } else {
                close_tag = xml_close_tag();
            }
            const std::size_t close_at =
                close_tag.has_value() ? buffer_.find(*close_tag) : std::string::npos;
            if (close_at == std::string::npos) {
                // JSON supports useful partial argument snapshots; XML parameters are only
                // announced once their closing tags make the complete JSON unambiguous.
                if (active_tagged_protocol_ == TaggedProtocol::kJson) {
                    announce_partial(buffer_, callbacks);
                }
                return;
            }

            const std::string body = buffer_.substr(0, close_at);
            std::string complete_tagged_text;
            if (active_tagged_protocol_ == TaggedProtocol::kJson) {
                complete_tagged_text = std::string(kOpenTag) + body + *close_tag;
            } else {
                complete_tagged_text = body + *close_tag;
            }
            const std::vector<ParsedTextCall> parsed_calls = parse_tool_call_text(complete_tagged_text);
            const ParsedTextCall* parsed = parsed_calls.size() == 1 ? &parsed_calls.front() : nullptr;
            if (parsed != nullptr) {
                const std::string& key = required_stream_key(stream_key_);
                emit_snapshot(key, parsed->name, parsed->arguments, callbacks);
                callbacks.on_complete(CompletedCall{
                    key,
                    parsed->id,
                    parsed->name,
                    parsed->arguments,
                });
            } else {
                announce_partial(body, callbacks);
                callbacks.on_malformed("Tagged tool call was not valid complete JSON");
            }
            buffer_.erase(0, close_at + close_tag->size());
            reset_after_call();
            mode_ = Mode::kNormal;
            can_probe_bare_json_ = false;
            continue;
        }

        // Mode::kBareJsonCall
        announce_partial(buffer_, callbacks);
        return;
    }
}

void StreamingTextToolCallParser::complete_bare_json(const Callbacks& callbacks) {
    const std::vector<ParsedTextCall> parsed = parse_tool_call_text(buffer_);
    if (!parsed.empty()) {
        for (std::size_t index = 0; index < parsed.size(); ++index) {
            std::string key;
            if (index == 0) {
                key = stream_key_.has_value() ? *stream_key_ : new_stream_key();
            } else {
                key = new_stream_key();
            }
            emit_snapshot(key, parsed[index].name, parsed[index].arguments, callbacks);
            callbacks.on_complete(CompletedCall{
                key,
                parsed[index].id,
                parsed[index].name,
                parsed[index].arguments,
            });
        }
        return;
    }
    announce_partial(buffer_, callbacks);
    callbacks.on_malformed("Provider ended before the JSON tool call was complete");
}

void StreamingTextToolCallParser::announce_partial(
    const std::string& body,
    const Callbacks& callbacks) {
    std::optional<std::string> name;
    if (const auto extracted = name_field_capture(body)) {
        const json::ParseResult decoded = json::parse("\"" + *extracted + "\"");
        if (decoded.value.has_value() && decoded.value->kind == json::Kind::String &&
            !json::is_blank(decoded.value->text)) {
            name = decoded.value->text;
        }
    }
    if (!name.has_value()) name = announced_name_;
    if (!name.has_value()) return;
    const std::optional<std::size_t> arguments_start = arguments_key_start(body);
    if (!arguments_start.has_value()) return;
    const std::string arguments = partial_arguments(body.substr(*arguments_start));
    announced_name_ = *name;
    announced_arguments_ = arguments;
    emit_snapshot(required_stream_key(stream_key_), *name, arguments, callbacks);
}

void StreamingTextToolCallParser::emit_snapshot(
    const std::string& key,
    const std::string& name,
    const std::string& arguments,
    const Callbacks& callbacks) {
    const Snapshot snapshot{key, name, arguments};
    if (!last_snapshot_.has_value() || !(*last_snapshot_ == snapshot)) {
        last_snapshot_ = snapshot;
        callbacks.on_update(snapshot);
    }
}

void StreamingTextToolCallParser::begin_call(Mode next_mode, const Callbacks& callbacks) {
    mode_ = next_mode;
    stream_key_ = new_stream_key();
    announced_name_ = std::nullopt;
    announced_arguments_.clear();
    last_snapshot_ = std::nullopt;
    emit_snapshot(*stream_key_, "", "", callbacks);
}

void StreamingTextToolCallParser::reset_after_call() {
    active_tagged_protocol_ = TaggedProtocol::kJson;
    stream_key_ = std::nullopt;
    announced_name_ = std::nullopt;
    announced_arguments_.clear();
    last_snapshot_ = std::nullopt;
}

std::optional<std::pair<std::size_t, StreamingTextToolCallParser::TaggedProtocol>>
StreamingTextToolCallParser::find_tagged_start() const {
    std::vector<std::pair<std::size_t, TaggedProtocol>> candidates;
    const std::size_t json_at = detail::find_ci(buffer_, kOpenTag);
    if (json_at != std::string::npos) candidates.emplace_back(json_at, TaggedProtocol::kJson);
    const std::size_t xml_at = detail::find_ci(buffer_, kInvokeOpen);
    if (xml_at != std::string::npos) candidates.emplace_back(xml_at, TaggedProtocol::kXml);
    const std::size_t antml_at = detail::find_ci(buffer_, kAntmlInvokeOpen);
    if (antml_at != std::string::npos) candidates.emplace_back(antml_at, TaggedProtocol::kXml);
    if (candidates.empty()) return std::nullopt;
    std::size_t best = 0;
    for (std::size_t index = 1; index < candidates.size(); ++index) {
        if (candidates[index].first < candidates[best].first) best = index;
    }
    return candidates[best];
}

std::optional<std::string> StreamingTextToolCallParser::xml_close_tag() const {
    const auto find_close = [&](std::string_view tag) -> std::optional<std::string> {
        std::size_t index = 0;
        while (index < buffer_.size()) {
            const std::size_t at = detail::find_ci(buffer_, tag, index);
            if (at == std::string::npos) return std::nullopt;
            std::size_t cursor = at + tag.size();
            while (cursor < buffer_.size() && detail::is_java_space(buffer_[cursor])) ++cursor;
            if (cursor < buffer_.size() && buffer_[cursor] == '>') {
                return buffer_.substr(at, cursor + 1 - at);
            }
            index = at + 1;
        }
        return std::nullopt;
    };
    if (detail::contains_ci(buffer_, kAntmlInvokeClose)) return find_close(kAntmlInvokeClose);
    if (detail::contains_ci(buffer_, kInvokeClose)) return find_close(kInvokeClose);
    return std::nullopt;
}

std::size_t StreamingTextToolCallParser::longest_tag_prefix_suffix() const {
    const std::vector<std::string_view> prefixes = {kOpenTag, kInvokeOpen, kAntmlInvokeOpen};
    std::size_t max_prefix = 0;
    for (const std::string_view prefix : prefixes) {
        if (prefix.size() > max_prefix) max_prefix = prefix.size();
    }
    const std::size_t max_length = std::min(buffer_.size(), max_prefix - 1);
    for (std::size_t length = max_length; length >= 1; --length) {
        const std::string_view suffix(buffer_.data() + buffer_.size() - length, length);
        for (const std::string_view prefix : prefixes) {
            if (detail::starts_with_ci(prefix, suffix)) return length;
        }
    }
    return 0;
}

}  // namespace agora::core::stream_normalization
