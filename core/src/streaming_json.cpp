// Port of ui/chat/message/StreamingJsonDocument.kt and StreamingToolArgumentHints.kt.
//
// Both Kotlin sources operate on String, so every cursor, errorOffset, length and slice below is
// a UTF-16 code unit. The parser walks a std::u16string and only converts to UTF-8 when a value
// leaves the port (toString()/describe or the Hints boundary); an unpaired surrogate becomes '?'
// exactly like the JVM UTF-8 encoder.
#include "agora/core/streaming_json.hpp"

#include <cstdint>
#include <utility>

#include "agora/core/json.hpp"
#include "agora/core/unicode.hpp"
#include "utf16.hpp"

namespace agora::core::streaming_json {
namespace {

using U16 = std::u16string;

struct ParsedNode {
    std::optional<Node> node;
    bool complete = false;
};

struct ParsedString {
    U16 content;
    bool complete = false;
};

bool is_json_whitespace(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\r' || c == u'\n';
}

bool is_ascii_digit(char16_t c) { return c >= u'0' && c <= u'9'; }

/**
 * Kotlin's `Char.digitToIntOrNull(16)` accepts Unicode digit forms through Character.digit.
 * JSON `\u` escapes are always written with ASCII hex digits by every encoder, and the golden
 * scenarios pin ASCII; the port stays on ASCII and would reject exotic digit forms.
 */
int hex_digit(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'f') return c - u'a' + 10;
    if (c >= u'A' && c <= u'F') return c - u'A' + 10;
    return -1;
}

/**
 * Kotlin String.isBlank() for a UTF-16 buffer: Character.isWhitespace || isSpaceChar.
 */
bool is_blank_utf16(const U16& text) {
    for (const char16_t unit : text) {
        if (!unicode::is_kotlin_whitespace(unit)) return false;
    }
    return true;
}

class Parser {
public:
    explicit Parser(std::u16string_view source) : source_(source) {}

    Document parse_document() {
        skip_whitespace();
        if (at_end()) {
            // Mirrors the early return that leaves roots at its default (empty) value.
            Document document;
            document.root = std::nullopt;
            document.status = Status::Incomplete;
            return document;
        }

        std::vector<Node> roots;
        Status status = Status::Incomplete;
        while (!at_end() && !error_offset_.has_value()) {
            ParsedNode parsed = parse_value();
            if (parsed.node.has_value()) roots.push_back(std::move(*parsed.node));
            if (error_offset_.has_value()) {
                status = Status::Invalid;
                break;
            }
            if (!parsed.complete) {
                status = Status::Incomplete;
                break;
            }

            const int separator_length = skip_whitespace();
            if (at_end()) {
                status = Status::Complete;
                break;
            }
            // Concatenated values without an actual JSON-whitespace boundary remain invalid.
            if (separator_length == 0) {
                fail();
                status = Status::Invalid;
                break;
            }
        }
        if (error_offset_.has_value()) status = Status::Invalid;

        Document document;
        if (!roots.empty()) document.root = roots.front();
        document.status = status;
        document.error_offset = error_offset_;
        document.roots = std::move(roots);
        return document;
    }

private:
    std::u16string_view source_;
    std::size_t cursor_ = 0;
    std::optional<int> error_offset_;

    bool at_end() const { return cursor_ >= source_.size(); }

    void fail() { fail(static_cast<int>(cursor_)); }

    void fail(int offset) {
        if (!error_offset_.has_value()) error_offset_ = offset;
    }

    int skip_whitespace() {
        const std::size_t start = cursor_;
        while (!at_end() && is_json_whitespace(source_[cursor_])) ++cursor_;
        return static_cast<int>(cursor_ - start);
    }

    ParsedNode parse_value() {
        skip_whitespace();
        if (at_end()) return ParsedNode{std::nullopt, false};
        switch (source_[cursor_]) {
            case u'{':
                return parse_object();
            case u'[':
                return parse_array();
            case u'"': {
                ParsedString parsed = parse_string();
                Node node;
                node.kind = Node::Kind::Scalar;
                node.content = std::move(parsed.content);
                node.scalar_kind = ScalarKind::String;
                node.complete = parsed.complete;
                return ParsedNode{std::move(node), parsed.complete};
            }
            case u't':
                return parse_keyword(u"true", ScalarKind::Boolean);
            case u'f':
                return parse_keyword(u"false", ScalarKind::Boolean);
            case u'n':
                return parse_keyword(u"null", ScalarKind::Null);
            case u'-':
                return parse_number();
            default:
                if (source_[cursor_] >= u'0' && source_[cursor_] <= u'9') return parse_number();
                fail();
                return ParsedNode{std::nullopt, false};
        }
    }

    static ParsedNode object_node(std::vector<Entry> entries, bool complete) {
        Node node;
        node.kind = Node::Kind::Object;
        node.entries = std::move(entries);
        node.complete = complete;
        return ParsedNode{std::move(node), complete};
    }

    static ParsedNode array_node(std::vector<Node> values, bool complete) {
        Node node;
        node.kind = Node::Kind::Array;
        node.values = std::move(values);
        node.complete = complete;
        return ParsedNode{std::move(node), complete};
    }

    ParsedNode parse_object() {
        ++cursor_;  // {
        std::vector<Entry> entries;
        skip_whitespace();
        if (at_end()) return object_node(std::move(entries), false);
        if (source_[cursor_] == u'}') {
            ++cursor_;
            return object_node(std::move(entries), true);
        }

        while (!error_offset_.has_value()) {
            if (at_end()) return object_node(std::move(entries), false);
            if (source_[cursor_] != u'"') {
                fail();
                return object_node(std::move(entries), false);
            }
            const ParsedString key = parse_string();
            if (!key.complete) {
                entries.push_back(Entry{key.content, false, std::nullopt});
                return object_node(std::move(entries), false);
            }

            skip_whitespace();
            if (at_end()) {
                entries.push_back(Entry{key.content, true, std::nullopt});
                return object_node(std::move(entries), false);
            }
            if (source_[cursor_] != u':') {
                fail();
                entries.push_back(Entry{key.content, true, std::nullopt});
                return object_node(std::move(entries), false);
            }
            ++cursor_;
            skip_whitespace();
            if (at_end()) {
                entries.push_back(Entry{key.content, true, std::nullopt});
                return object_node(std::move(entries), false);
            }

            ParsedNode value = parse_value();
            Entry entry;
            entry.key = key.content;
            entry.key_complete = true;
            entry.value = std::move(value.node);
            entries.push_back(std::move(entry));
            if (error_offset_.has_value() || !value.complete) {
                return object_node(std::move(entries), false);
            }

            skip_whitespace();
            if (at_end()) return object_node(std::move(entries), false);
            switch (source_[cursor_]) {
                case u'}':
                    ++cursor_;
                    return object_node(std::move(entries), true);
                case u',':
                    ++cursor_;
                    skip_whitespace();
                    if (at_end()) return object_node(std::move(entries), false);
                    break;
                default:
                    fail();
                    return object_node(std::move(entries), false);
            }
        }
        return object_node(std::move(entries), false);
    }

    ParsedNode parse_array() {
        ++cursor_;  // [
        std::vector<Node> values;
        skip_whitespace();
        if (at_end()) return array_node(std::move(values), false);
        if (source_[cursor_] == u']') {
            ++cursor_;
            return array_node(std::move(values), true);
        }

        while (!error_offset_.has_value()) {
            if (at_end()) return array_node(std::move(values), false);
            ParsedNode value = parse_value();
            if (value.node.has_value()) values.push_back(std::move(*value.node));
            if (error_offset_.has_value() || !value.complete) {
                return array_node(std::move(values), false);
            }

            skip_whitespace();
            if (at_end()) return array_node(std::move(values), false);
            switch (source_[cursor_]) {
                case u']':
                    ++cursor_;
                    return array_node(std::move(values), true);
                case u',':
                    ++cursor_;
                    skip_whitespace();
                    if (at_end()) return array_node(std::move(values), false);
                    break;
                default:
                    fail();
                    return array_node(std::move(values), false);
            }
        }
        return array_node(std::move(values), false);
    }

    ParsedString parse_string() {
        ++cursor_;  // opening quote
        U16 content;
        while (!at_end() && !error_offset_.has_value()) {
            const char16_t unit = source_[cursor_++];
            if (unit == u'"') return ParsedString{std::move(content), true};
            if (unit == u'\\') {
                if (at_end()) return ParsedString{std::move(content), false};
                const char16_t escape = source_[cursor_++];
                switch (escape) {
                    case u'"':
                        content.push_back(u'"');
                        break;
                    case u'\\':
                        content.push_back(u'\\');
                        break;
                    case u'/':
                        content.push_back(u'/');
                        break;
                    case u'b':
                        content.push_back(static_cast<char16_t>(0x08));
                        break;
                    case u'f':
                        content.push_back(static_cast<char16_t>(0x0C));
                        break;
                    case u'n':
                        content.push_back(u'\n');
                        break;
                    case u'r':
                        content.push_back(u'\r');
                        break;
                    case u't':
                        content.push_back(u'\t');
                        break;
                    case u'u': {
                        int value = 0;
                        int digits = 0;
                        while (digits < 4 && !at_end()) {
                            const int digit = hex_digit(source_[cursor_]);
                            if (digit < 0) {
                                fail();
                                return ParsedString{std::move(content), false};
                            }
                            value = value * 16 + digit;
                            ++cursor_;
                            ++digits;
                        }
                        if (digits < 4) return ParsedString{std::move(content), false};
                        content.push_back(static_cast<char16_t>(value));
                        break;
                    }
                    default:
                        fail(static_cast<int>(cursor_) - 1);
                        return ParsedString{std::move(content), false};
                }
                continue;
            }
            if (unit < 0x20) {
                fail(static_cast<int>(cursor_) - 1);
                return ParsedString{std::move(content), false};
            }
            content.push_back(unit);
        }
        return ParsedString{std::move(content), false};
    }

    ParsedNode parse_keyword(std::u16string_view keyword, ScalarKind kind) {
        const std::size_t start = cursor_;
        std::size_t keyword_index = 0;
        while (keyword_index < keyword.size() && !at_end()) {
            if (source_[cursor_] != keyword[keyword_index]) {
                fail();
                break;
            }
            ++cursor_;
            ++keyword_index;
        }
        const bool complete = !error_offset_.has_value() && keyword_index == keyword.size();
        Node node;
        node.kind = Node::Kind::Scalar;
        node.content = U16(source_.substr(start, cursor_ - start));
        node.scalar_kind = kind;
        node.complete = complete;
        return ParsedNode{std::move(node), complete};
    }

    ParsedNode number_node(std::size_t start, bool complete) {
        Node node;
        node.kind = Node::Kind::Scalar;
        node.content = U16(source_.substr(start, cursor_ - start));
        node.scalar_kind = ScalarKind::Number;
        node.complete = complete;
        return ParsedNode{std::move(node), complete};
    }

    ParsedNode parse_number() {
        const std::size_t start = cursor_;
        if (source_[cursor_] == u'-') {
            ++cursor_;
            if (at_end()) return number_node(start, false);
        }

        if (at_end()) return number_node(start, false);
        if (source_[cursor_] == u'0') {
            ++cursor_;
        } else if (source_[cursor_] >= u'1' && source_[cursor_] <= u'9') {
            ++cursor_;
            while (!at_end() && is_ascii_digit(source_[cursor_])) ++cursor_;
        } else {
            fail();
            return number_node(start, false);
        }

        if (!at_end() && source_[cursor_] == u'.') {
            ++cursor_;
            if (at_end()) return number_node(start, false);
            if (!is_ascii_digit(source_[cursor_])) {
                fail();
                return number_node(start, false);
            }
            while (!at_end() && is_ascii_digit(source_[cursor_])) ++cursor_;
        }

        if (!at_end() && (source_[cursor_] == u'e' || source_[cursor_] == u'E')) {
            ++cursor_;
            if (!at_end() && (source_[cursor_] == u'+' || source_[cursor_] == u'-')) ++cursor_;
            if (at_end()) return number_node(start, false);
            if (!is_ascii_digit(source_[cursor_])) {
                fail();
                return number_node(start, false);
            }
            while (!at_end() && is_ascii_digit(source_[cursor_])) ++cursor_;
        }
        return number_node(start, true);
    }
};

Document parse_u16(std::u16string_view source) { return Parser(source).parse_document(); }

// ---- Kotlin data-class toString() ------------------------------------------------------------

void append_node(std::string& out, const Node& node);

void append_entry(std::string& out, const Entry& entry) {
    out += "StreamingJsonEntry(key=";
    out += utf16::to_utf8(entry.key);
    out += ", keyComplete=";
    out += entry.key_complete ? "true" : "false";
    out += ", value=";
    if (entry.value.has_value()) {
        append_node(out, *entry.value);
    } else {
        out += "null";
    }
    out += ")";
}

void append_node(std::string& out, const Node& node) {
    switch (node.kind) {
        case Node::Kind::Object: {
            out += "StreamingJsonObject(entries=[";
            for (std::size_t i = 0; i < node.entries.size(); ++i) {
                if (i != 0) out += ", ";
                append_entry(out, node.entries[i]);
            }
            out += "], complete=";
            out += node.complete ? "true" : "false";
            out += ")";
            break;
        }
        case Node::Kind::Array: {
            out += "StreamingJsonArray(values=[";
            for (std::size_t i = 0; i < node.values.size(); ++i) {
                if (i != 0) out += ", ";
                append_node(out, node.values[i]);
            }
            out += "], complete=";
            out += node.complete ? "true" : "false";
            out += ")";
            break;
        }
        case Node::Kind::Scalar: {
            out += "StreamingJsonScalar(content=";
            out += utf16::to_utf8(node.content);
            out += ", kind=";
            switch (node.scalar_kind) {
                case ScalarKind::String:
                    out += "STRING";
                    break;
                case ScalarKind::Number:
                    out += "NUMBER";
                    break;
                case ScalarKind::Boolean:
                    out += "BOOLEAN";
                    break;
                case ScalarKind::Null:
                    out += "NULL";
                    break;
            }
            out += ", complete=";
            out += node.complete ? "true" : "false";
            out += ")";
            break;
        }
    }
}

std::optional<std::string> normalize_u16(const std::optional<U16>& value, int max_characters) {
    if (!value.has_value()) return std::nullopt;
    if (max_characters <= 0) return std::nullopt;  // Kotlin requires this; unreachable in the app

    // replace(Regex("\\s+"), " "): java.util.regex \s is exactly [ \t\n\x0B\f\r].
    U16 collapsed;
    collapsed.reserve(value->size());
    bool in_space_run = false;
    for (const char16_t unit : *value) {
        const bool is_space = unit == u' ' || unit == u'\t' || unit == u'\n' ||
                              unit == u'\x0B' || unit == u'\f' || unit == u'\r';
        if (is_space) {
            if (!in_space_run) collapsed.push_back(u' ');
            in_space_run = true;
        } else {
            collapsed.push_back(unit);
            in_space_run = false;
        }
    }

    // trim(): Kotlin Character.isWhitespace || Character.isSpaceChar.
    std::size_t begin = 0;
    std::size_t end = collapsed.size();
    while (begin < end && unicode::is_kotlin_whitespace(collapsed[begin])) ++begin;
    while (end > begin && unicode::is_kotlin_whitespace(collapsed[end - 1])) --end;

    if (begin == end) return std::nullopt;
    const std::size_t length = end - begin;
    if (static_cast<std::size_t>(max_characters) < length) {
        U16 truncated = collapsed.substr(begin, static_cast<std::size_t>(max_characters) - 1);
        truncated.push_back(static_cast<char16_t>(0x2026));  // HORIZONTAL ELLIPSIS
        return utf16::to_utf8(truncated);
    }
    return utf16::to_utf8(collapsed.substr(begin, length));
}

const Node* first_entry_value(const Node& object, std::u16string_view key) {
    for (const Entry& entry : object.entries) {
        if (entry.key_complete && entry.key == key) return entry.value.has_value() ? &*entry.value : nullptr;
    }
    return nullptr;
}

std::optional<U16> scalar(const Node& object, std::u16string_view key) {
    const Node* value = first_entry_value(object, key);
    if (value == nullptr || value->kind != Node::Kind::Scalar) return std::nullopt;
    return value->content;
}

std::optional<std::string> utf8_or_null(const std::optional<U16>& value) {
    if (!value.has_value()) return std::nullopt;
    return utf16::to_utf8(*value);
}

bool is_read_kind(ToolKind kind) {
    return kind == ToolKind::MemoryRead || kind == ToolKind::SkillRead;
}

}  // namespace

std::optional<ToolKind> tool_kind_from_name(std::string_view name) {
    static const std::pair<std::string_view, ToolKind> kNames[] = {
        {"MEMORY_LIST", ToolKind::MemoryList},
        {"MEMORY_READ", ToolKind::MemoryRead},
        {"MEMORY_CREATE", ToolKind::MemoryCreate},
        {"MEMORY_EDIT", ToolKind::MemoryEdit},
        {"MEMORY_DELETE", ToolKind::MemoryDelete},
        {"MEMORY_UPDATE_ACTIVE", ToolKind::MemoryUpdateActive},
        {"SKILL_LIST", ToolKind::SkillList},
        {"SKILL_READ", ToolKind::SkillRead},
        {"SKILL_CREATE", ToolKind::SkillCreate},
        {"SKILL_EDIT", ToolKind::SkillEdit},
        {"SKILL_DELETE", ToolKind::SkillDelete},
        {"WEB_SEARCH", ToolKind::WebSearch},
        {"WEB_FETCH", ToolKind::WebFetch},
        {"CONVERSATION_SEARCH", ToolKind::ConversationSearch},
        {"CONVERSATION_LIST", ToolKind::ConversationList},
        {"CONVERSATION_READ", ToolKind::ConversationRead},
        {"SHELL_LIST", ToolKind::ShellList},
        {"SHELL_EXECUTE", ToolKind::ShellExecute},
        {"SHELL_JOB_LIST", ToolKind::ShellJobList},
        {"SHELL_JOB_GET", ToolKind::ShellJobGet},
        {"SHELL_JOB_WAIT", ToolKind::ShellJobWait},
        {"SHELL_JOB_STOP", ToolKind::ShellJobStop},
        {"FILE_READ", ToolKind::FileRead},
        {"FILE_WRITE", ToolKind::FileWrite},
        {"FILE_EDIT", ToolKind::FileEdit},
        {"FILE_GLOB", ToolKind::FileGlob},
        {"FILE_GREP", ToolKind::FileGrep},
        {"IMAGE_VIEW", ToolKind::ImageView},
        {"IMAGE_GENERATE", ToolKind::ImageGenerate},
        {"TASK_CREATE", ToolKind::TaskCreate},
        {"TASK_LIST", ToolKind::TaskList},
        {"TASK_DELETE", ToolKind::TaskDelete},
        {"LOOP_START", ToolKind::LoopStart},
        {"LOOP_STOP", ToolKind::LoopStop},
        {"ASK_USER", ToolKind::AskUser},
        {"MCP", ToolKind::Mcp},
        {"UNKNOWN", ToolKind::Unknown},
    };
    for (const auto& [candidate, kind] : kNames) {
        if (candidate == name) return kind;
    }
    return std::nullopt;
}

Document parse(std::string_view source_utf8) {
    const U16 source = utf16::from_utf8(source_utf8);
    return parse_u16(source);
}

std::optional<std::string> normalize_subject(const std::optional<std::string>& value,
                                             int max_characters) {
    if (!value.has_value()) return std::nullopt;
    return normalize_u16(utf16::from_utf8(*value), max_characters);
}

Hints resolve(ToolKind kind, const std::optional<std::string>& raw_arguments_utf8) {
    Hints hints;
    if (!raw_arguments_utf8.has_value() || json::is_blank(*raw_arguments_utf8)) {
        return hints;
    }

    U16 source = utf16::from_utf8(*raw_arguments_utf8);
    constexpr std::size_t kMaxPrefix = 4096;  // MAX_TOOL_ARGUMENT_HINT_PREFIX_CHARS
    if (source.size() > kMaxPrefix) source.resize(kMaxPrefix);

    const Document document = parse_u16(source);
    if (!document.root.has_value() || document.root->kind != Node::Kind::Object) return hints;
    const Node& root = *document.root;

    const bool read = is_read_kind(kind);
    const Node* names_array = first_entry_value(root, u"names");
    if (names_array != nullptr && names_array->kind != Node::Kind::Array) names_array = nullptr;

    std::vector<U16> names;
    if (names_array != nullptr) {
        for (const Node& value : names_array->values) {
            if (value.kind != Node::Kind::Scalar) continue;
            const bool keep = kind == ToolKind::MemoryRead
                                  ? !value.content.empty()
                                  : !is_blank_utf16(value.content);
            if (keep) names.push_back(value.content);
        }
    }

    const bool names_selected =
        read && names_array != nullptr &&
        (!names_array->complete ||
         (kind == ToolKind::MemoryRead ? !names_array->values.empty() : !names.empty()));

    std::optional<U16> subject;
    switch (kind) {
        case ToolKind::MemoryRead:
        case ToolKind::SkillRead:
            if (names_selected) {
                if (names.size() == 1 && names_array->complete) subject = names.front();
            } else {
                subject = scalar(root, u"name");
            }
            break;
        case ToolKind::MemoryCreate:
        case ToolKind::MemoryEdit:
        case ToolKind::MemoryDelete:
        case ToolKind::SkillCreate:
        case ToolKind::SkillEdit:
        case ToolKind::SkillDelete:
            subject = scalar(root, u"name");
            break;
        case ToolKind::WebSearch:
        case ToolKind::ConversationSearch:
            subject = scalar(root, u"query");
            break;
        case ToolKind::WebFetch:
            subject = scalar(root, u"url");
            break;
        case ToolKind::ConversationRead:
            subject = scalar(root, u"conversation_id");
            break;
        case ToolKind::ShellExecute:
            subject = scalar(root, u"command");
            break;
        case ToolKind::ShellJobGet:
        case ToolKind::ShellJobWait:
        case ToolKind::ShellJobStop:
            subject = scalar(root, u"job_id");
            break;
        case ToolKind::FileRead:
        case ToolKind::FileWrite:
        case ToolKind::FileEdit:
        case ToolKind::ImageView:
            subject = scalar(root, u"path");
            break;
        case ToolKind::FileGlob:
        case ToolKind::FileGrep:
            subject = scalar(root, u"pattern");
            break;
        case ToolKind::ImageGenerate:
            subject = scalar(root, u"prompt");
            break;
        case ToolKind::TaskCreate:
            subject = scalar(root, u"name");
            break;
        case ToolKind::TaskDelete:
            subject = scalar(root, u"id_or_name");
            if (!subject.has_value()) subject = scalar(root, u"name");
            if (!subject.has_value()) subject = scalar(root, u"task_id");
            break;
        default:
            break;
    }

    hints.subject = subject.has_value() ? normalize_u16(subject, 120) : std::nullopt;
    if (const auto server = scalar(root, u"server")) hints.server = normalize_u16(server, 120);
    if (names_selected && names_array->complete) hints.count = static_cast<int>(names.size());
    hints.operation = utf8_or_null(scalar(root, u"operation"));
    if (const auto destination = scalar(root, u"new_name")) {
        hints.destination = normalize_u16(destination, 120);
    }
    return hints;
}

std::string describe(const Document& document) {
    std::string out = "StreamingJsonDocument(root=";
    if (document.root.has_value()) {
        append_node(out, *document.root);
    } else {
        out += "null";
    }
    out += ", status=";
    switch (document.status) {
        case Status::Incomplete:
            out += "INCOMPLETE";
            break;
        case Status::Complete:
            out += "COMPLETE";
            break;
        case Status::Invalid:
            out += "INVALID";
            break;
    }
    out += ", errorOffset=";
    out += document.error_offset.has_value() ? std::to_string(*document.error_offset) : "null";
    out += ", roots=[";
    for (std::size_t i = 0; i < document.roots.size(); ++i) {
        if (i != 0) out += ", ";
        append_node(out, document.roots[i]);
    }
    out += "])";
    return out;
}

std::string describe(const Hints& hints) {
    const auto text = [](const std::optional<std::string>& value) {
        return value.has_value() ? *value : std::string("null");
    };
    std::string out = "StreamingToolArgumentHints(subject=" + text(hints.subject) +
                      ", server=" + text(hints.server) + ", count=" +
                      (hints.count.has_value() ? std::to_string(*hints.count) : "null") +
                      ", operation=" + text(hints.operation) +
                      ", destination=" + text(hints.destination) + ")";
    return out;
}

}  // namespace agora::core::streaming_json
