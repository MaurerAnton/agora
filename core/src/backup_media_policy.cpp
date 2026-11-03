#include "agora/core/backup_media_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>

#include "agora/core/json.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::backup {
namespace {

namespace json = agora::core::json;
using json::Kind;

// ---------------------------------------------------------------------------------------------
// Writer: kotlinx's compact encoder with encodeDefaults = false
// ---------------------------------------------------------------------------------------------

/** kotlinx's string escaping: five short escapes, \u00xx for the rest of C0, UTF-8 untouched. */
void quoted(std::string& out, std::string_view text) {
    static const char* hex = "0123456789abcdef";
    out += '"';
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\b': out += "\\b"; continue;
            case '\t': out += "\\t"; continue;
            case '\n': out += "\\n"; continue;
            case '\f': out += "\\f"; continue;
            case '\r': out += "\\r"; continue;
            default: break;
        }
        if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

/** Builds compact JSON in document order; each value position emits its own separator. */
class JsonOut {
   public:
    void open_object() {
        separate();
        text_ += '{';
        fresh_.push_back(true);
    }
    void close_object() {
        text_ += '}';
        fresh_.pop_back();
    }
    void open_array() {
        separate();
        text_ += '[';
        fresh_.push_back(true);
    }
    void close_array() { text_ += ']'; fresh_.pop_back(); }

    void key(std::string_view name) {
        separate();
        quoted(text_, name);
        text_ += ':';
        pending_ = true;
    }

    void string_value(std::string_view value) {
        separate();
        quoted(text_, value);
    }
    void raw_value(std::string_view value) {
        separate();
        text_ += value;
    }
    void number_value(std::int64_t value) {
        separate();
        text_ += std::to_string(value);
    }

    /** Emits a nullable field unless it holds the declared default. */
    template <class T, class Emit>
    void optional_field(std::string_view name, const std::optional<T>& value, Emit emit) {
        if (!value.has_value()) return;
        key(name);
        emit(*value);
    }

    void string_field(std::string_view name, std::string_view value) {
        key(name);
        string_value(value);
    }
    void number_field(std::string_view name, std::int64_t value) {
        key(name);
        number_value(value);
    }
    void raw_field(std::string_view name, std::string_view value) {
        key(name);
        raw_value(value);
    }
    void bool_field(std::string_view name, bool value, bool fallback) {
        if (value == fallback) return;
        key(name);
        raw_value(value ? "true" : "false");
    }

    std::string take() const { return text_; }

   private:
    void separate() {
        if (pending_) {  // the value that follows a key writes straight after the colon
            pending_ = false;
            return;
        }
        if (fresh_.empty()) return;
        if (!fresh_.back()) text_ += ',';
        fresh_.back() = false;
    }

    std::string text_;
    std::vector<bool> fresh_;
    bool pending_ = false;
};

void put_string(JsonOut& out, std::string_view name, const std::optional<std::string>& value) {
    out.optional_field(name, value, [&out](const std::string& v) { out.string_value(v); });
}

void put_int(JsonOut& out, std::string_view name, const std::optional<int>& value) {
    out.optional_field(name, value, [&out](int v) { out.number_value(v); });
}

void put_long(JsonOut& out, std::string_view name, const std::optional<std::int64_t>& value) {
    out.optional_field(name, value, [&out](std::int64_t v) { out.number_value(v); });
}

void put_storage(JsonOut& out, std::string_view name, AttachmentStorage value) {
    if (value == AttachmentStorage::app_private) return;
    out.key(name);
    switch (value) {
        case AttachmentStorage::app_private: out.string_value("app_private"); break;
        case AttachmentStorage::local_sandbox_pending: out.string_value("local_sandbox_pending"); break;
        case AttachmentStorage::local_sandbox_runtime: out.string_value("local_sandbox_runtime"); break;
    }
}

void put_import_state(JsonOut& out, std::string_view name, AttachmentImportState value) {
    if (value == AttachmentImportState::ready) return;
    out.key(name);
    switch (value) {
        case AttachmentImportState::processing: out.string_value("processing"); break;
        case AttachmentImportState::ready: out.string_value("ready"); break;
        case AttachmentImportState::failed: out.string_value("failed"); break;
    }
}

void put_string_list(JsonOut& out, std::string_view name,
                     const std::optional<std::vector<std::string>>& value) {
    // The declared default is null, so an explicitly empty list is still encoded as [].
    if (!value.has_value()) return;
    out.key(name);
    out.open_array();
    for (const std::string& element : *value) out.string_value(element);
    out.close_array();
}

void put_raw_list(JsonOut& out, std::string_view name, const std::vector<std::string>& value) {
    if (value.empty()) return;
    out.key(name);
    out.open_array();
    for (const std::string& element : value) out.raw_value(element);
    out.close_array();
}

// ---------------------------------------------------------------------------------------------
// Reader: kotlinx's decoder with ignoreUnknownKeys = true
// ---------------------------------------------------------------------------------------------

/** Any type mismatch kotlinx would throw on; the policy funnels those through runCatching. */
struct DecodeError {};

[[noreturn]] void decode_fail() { throw DecodeError{}; }

bool is_json_null(const json::Value& value) {
    return value.kind == Kind::Literal && value.text == "null";
}

/** Last assignment wins, which is how a duplicate JSON key resolves on the Kotlin side. */
const json::Value* lookup(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

/** Read for a nullable property: absent and an explicit null both yield nullopt. */
std::optional<std::string> read_string(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value)) return std::nullopt;
    if (value->kind != Kind::String) decode_fail();
    return value->text;
}

/** Read for a non-nullable property with a default: absent takes the default, null fails. */
std::string read_non_null_string(const json::Value& object, std::string_view name,
                                 std::string fallback) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr) return fallback;
    if (is_json_null(*value) || value->kind != Kind::String) decode_fail();
    return value->text;
}

std::string read_required_string(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value) || value->kind != Kind::String) decode_fail();
    return value->text;
}

bool parse_int_literal(const std::string& raw, std::int64_t& out) {
    if (raw.empty()) return false;
    std::size_t at = 0;
    bool negative = false;
    if (raw[0] == '+' || raw[0] == '-') {
        negative = raw[0] == '-';
        at = 1;
    }
    if (at >= raw.size()) return false;
    const std::uint64_t limit = negative ? 9223372036854775808ull : 9223372036854775807ull;
    std::uint64_t magnitude = 0;
    for (; at < raw.size(); ++at) {
        if (raw[at] < '0' || raw[at] > '9') return false;
        const auto digit = static_cast<std::uint64_t>(raw[at] - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (!negative) {
        out = static_cast<std::int64_t>(magnitude);
    } else if (magnitude == limit) {
        out = INT64_MIN;
    } else {
        out = -static_cast<std::int64_t>(magnitude);
    }
    return true;
}

std::optional<int> read_int(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value)) return std::nullopt;
    if (value->kind != Kind::Literal) decode_fail();
    std::int64_t parsed = 0;
    if (!parse_int_literal(value->text, parsed) || parsed < -2147483648ll || parsed > 2147483647ll) {
        decode_fail();
    }
    return static_cast<int>(parsed);
}

std::optional<std::int64_t> read_long(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value)) return std::nullopt;
    if (value->kind != Kind::Literal) decode_fail();
    std::int64_t parsed = 0;
    if (!parse_int_literal(value->text, parsed)) decode_fail();
    return parsed;
}

bool read_bool(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value)) return fallback;
    if (value->kind != Kind::Literal) decode_fail();
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    decode_fail();
}

AttachmentStorage read_storage(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr) return AttachmentStorage::app_private;
    if (is_json_null(*value) || value->kind != Kind::String) decode_fail();
    if (value->text == "app_private") return AttachmentStorage::app_private;
    if (value->text == "local_sandbox_pending") return AttachmentStorage::local_sandbox_pending;
    if (value->text == "local_sandbox_runtime") return AttachmentStorage::local_sandbox_runtime;
    decode_fail();
}

/** A non-nullable list property: absent takes the default, null fails the decode. */
std::vector<json::Value> read_array(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr) return {};
    if (is_json_null(*value) || value->kind != Kind::Array) decode_fail();
    return value->elements;
}

// ---------------------------------------------------------------------------------------------
// Model codecs
// ---------------------------------------------------------------------------------------------

ToolImageAttachment decode_tool_image(const json::Value& value);

AttachmentItem decode_attachment_item(const json::Value& value) {
    if (value.kind != Kind::Object) decode_fail();
    AttachmentItem item;
    item.original_uri = read_string(value, "originalUri");
    item.type = read_required_string(value, "type");
    item.file_name = read_string(value, "file_name");
    item.mime_type = read_string(value, "mime_type");
    item.image_index = read_int(value, "image_index");
    item.page_count = read_int(value, "page_count");
    item.pixel_width = read_int(value, "pixel_width");
    item.pixel_height = read_int(value, "pixel_height");
    item.warning = read_string(value, "warning");
    item.text_content = read_string(value, "text_content");
    item.transcription = read_string(value, "transcription");
    item.storage = read_storage(value, "storage");
    item.sandbox_path = read_string(value, "sandbox_path");
    item.file_size = read_long(value, "file_size");
    item.unavailable = read_bool(value, "unavailable", false);
    for (const json::Value& element : read_array(value, "imageArtifacts")) {
        item.image_artifacts.push_back(decode_tool_image(element));
    }
    return item;
}

ToolImageAttachment decode_tool_image(const json::Value& value) {
    if (value.kind != Kind::Object) decode_fail();
    ToolImageAttachment image;
    image.path = read_required_string(value, "path");
    image.mime_type = read_required_string(value, "mimeType");
    const std::optional<std::int64_t> size = read_long(value, "sizeBytes");
    if (!size.has_value()) decode_fail();
    image.size_bytes = *size;
    image.width = read_int(value, "width");
    image.height = read_int(value, "height");
    image.policy_version = read_int(value, "policyVersion").value_or(0);
    image.unavailable = read_bool(value, "unavailable", false);
    return image;
}

MessageSegment decode_message_segment(const json::Value& value) {
    if (value.kind != Kind::Object) decode_fail();
    MessageSegment segment;
    segment.type = read_required_string(value, "type");
    segment.content = read_non_null_string(value, "content", "");
    segment.tool_name = read_string(value, "toolName");
    segment.tool_args = read_string(value, "toolArgs");
    segment.tool_result = read_string(value, "toolResult");
    segment.tool_call_id = read_string(value, "toolCallId");
    segment.signature = read_string(value, "signature");
    segment.signature_provider = read_string(value, "signatureProvider");
    segment.duration_ms = read_long(value, "durationMs");
    segment.tool_state = read_string(value, "toolState");
    segment.tool_progress = read_string(value, "toolProgress");
    segment.tool_target = read_string(value, "toolTarget");
    segment.tool_display_name = read_string(value, "toolDisplayName");
    segment.tool_result_text = read_string(value, "toolResultText");
    segment.tool_structured_result = read_string(value, "toolStructuredResult");

    for (const json::Value& element : read_array(value, "toolImages")) {
        segment.tool_images.push_back(decode_tool_image(element));
    }
    segment.tool_transcription = read_string(value, "toolTranscription");
    for (const json::Value& element : read_array(value, "responseOutputItems")) {
        if (element.kind != Kind::Object) decode_fail();
        segment.response_output_items.push_back(json::to_kotlin_string(element));
    }
    segment.response_output_item_provider = read_string(value, "responseOutputItemProvider");
    segment.error_code = read_string(value, "errorCode");
    return segment;
}

// ---------------------------------------------------------------------------------------------
// Kotlin helpers
// ---------------------------------------------------------------------------------------------

/** String.isNotBlank(). */
bool not_blank(std::string_view text) { return !json::is_blank(text); }

/** String.isNullOrBlank() on an optional string. */
bool null_or_blank(const std::optional<std::string>& text) {
    return !text.has_value() || json::is_blank(*text);
}

/** String.substringBefore(delimiter): the whole string when the delimiter is absent. */
std::string substring_before(std::string_view text, char delimiter) {
    const std::size_t at = text.find(delimiter);
    return std::string(at == std::string_view::npos ? text : text.substr(0, at));
}

/** String.trimEnd(vararg chars). */
std::string trim_end(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == '/' || text[end - 1] == '\\')) --end;
    return std::string(text.substr(0, end));
}

/** String.substringAfterLast(delimiter): the whole string when the delimiter is absent. */
std::string substring_after_last(std::string_view text, char delimiter) {
    const std::size_t at = text.rfind(delimiter);
    return std::string(at == std::string_view::npos ? text : text.substr(at + 1));
}

/** NativeBackupMediaPolicy.sourceFileName. */
std::optional<std::string> source_file_name(std::string_view source) {
    const std::string clean =
        trim_end(substring_before(substring_before(source, '?'), '#'));
    const std::string name =
        substring_after_last(substring_after_last(clean, '/'), '\\');
    return not_blank(name) ? std::optional<std::string>(name) : std::nullopt;
}

/** listOfNotNull(localPath, uri.takeIf(String::isNotBlank)) for the file-name candidates. */
std::vector<std::string> file_name_candidates(const SelectedAttachment& attachment) {
    std::vector<std::string> sources;
    if (attachment.local_path.has_value()) sources.push_back(*attachment.local_path);
    if (!null_or_blank(attachment.uri)) sources.push_back(attachment.uri);
    return sources;
}

/** listOfNotNull(localPath?.takeIf(String::isNotBlank), uri.takeIf(String::isNotBlank)). */
std::vector<std::string> primary_candidates(const SelectedAttachment& attachment) {
    std::vector<std::string> sources;
    if (!null_or_blank(attachment.local_path)) sources.push_back(*attachment.local_path);
    if (!null_or_blank(attachment.uri)) sources.push_back(attachment.uri);
    return sources;
}

/** Int.coerceAtLeast(1) over the nullable page count. */
int coerced_count(const std::optional<int>& page_count) {
    if (!page_count.has_value()) return 1;
    return std::max(1, *page_count);
}

/** ImageArtifact.asUnavailablePlaceholder: keeps the occurrence and any known bounds. */
ToolImageAttachment as_unavailable_placeholder(const ToolImageAttachment& artifact) {
    ToolImageAttachment out = artifact;
    out.path.clear();
    out.size_bytes = 0;
    out.unavailable = true;
    return out;
}

/** unmeasuredArtifact: a slot without measured facts; the MIME type must be an image one. */
ToolImageAttachment unmeasured_artifact(const std::optional<std::string>& mime_type,
                                        const std::optional<int>& width,
                                        const std::optional<int>& height) {
    ToolImageAttachment artifact;
    const bool image_mime = mime_type.has_value() && mime_type->rfind("image/", 0) == 0;
    artifact.mime_type = image_mime ? *mime_type : std::string("application/octet-stream");
    artifact.width = width;
    artifact.height = height;
    return artifact;
}

/** attachmentItems(): the decoded meta plus legacy source rows it does not claim. */
std::vector<AttachmentItem> attachment_items(const std::optional<std::string>& raw,
                                             const std::map<int, std::string>& indexed_sources) {
    std::optional<AttachmentMeta> decoded;
    if (raw.has_value()) decoded = decode_attachment_meta(*raw);

    std::set<int> claimed;
    if (decoded.has_value()) {
        for (const AttachmentItem& item : decoded->items) {
            if (!item.image_index.has_value()) continue;
            const int count = coerced_count(item.page_count);
            for (int offset = 0; offset < count; ++offset) claimed.insert(*item.image_index + offset);
        }
    }

    std::vector<AttachmentItem> items;
    if (decoded.has_value()) items = decoded->items;
    for (const auto& [index, source] : indexed_sources) {
        if (claimed.count(index) != 0) continue;
        AttachmentItem legacy;
        legacy.original_uri = source;
        legacy.type = "image";
        legacy.file_name = source_file_name(source);
        legacy.image_index = index;
        items.push_back(std::move(legacy));
    }
    return items;
}

/** rewriteMessageArtifacts: one artifact per page/frame slot, missing slots included. */
std::vector<ToolImageAttachment> rewrite_message_artifacts(
    const AttachmentItem& item, const std::map<int, int>& old_to_new_image_index,
    const std::map<int, std::string>& source_table, const PathLookup& rewrite_path,
    bool legacy_mapped_paths) {
    const std::size_t page_slots =
        item.image_index.has_value() ? static_cast<std::size_t>(coerced_count(item.page_count)) : 0;
    const std::size_t count = std::max(item.image_artifacts.size(), page_slots);

    std::vector<ToolImageAttachment> artifacts;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const ToolImageAttachment recorded =
            offset < item.image_artifacts.size()
                ? item.image_artifacts[offset]
                : unmeasured_artifact(item.mime_type,
                                      count == 1 ? item.pixel_width : std::nullopt,
                                      count == 1 ? item.pixel_height : std::nullopt);
        std::optional<int> index;
        if (item.image_index.has_value()) index = *item.image_index + static_cast<int>(offset);
        std::string source = recorded.path;
        if (index.has_value()) {
            const auto found = source_table.find(*index);
            if (found != source_table.end()) source = found->second;
        }
        std::optional<std::string> target;
        if (index.has_value() && old_to_new_image_index.find(*index) == old_to_new_image_index.end()) {
            target = std::nullopt;
        } else if (legacy_mapped_paths) {
            if (index.has_value()) {
                const auto found = source_table.find(*index);
                if (found != source_table.end() && not_blank(found->second)) target = found->second;
            }
        } else if (not_blank(source)) {
            target = rewrite_path(source);
        }
        if (recorded.unavailable || !target.has_value() || json::is_blank(*target)) {
            artifacts.push_back(as_unavailable_placeholder(recorded));
        } else {
            ToolImageAttachment rewritten = recorded;
            rewritten.path = *target;
            artifacts.push_back(std::move(rewritten));
        }
    }
    return artifacts;
}

/** rewriteDraftMedia: remaps a draft payload for the archive in either direction. */
SelectedAttachment rewrite_draft_media(const SelectedAttachment& attachment,
                                       const std::optional<std::pair<std::string, std::string>>& primary,
                                       const PathLookup& rewrite_path,
                                       const MissingResource& on_missing) {
    const auto rewrite = [&](const std::optional<std::vector<std::string>>& paths)
        -> std::optional<std::vector<std::string>> {
        if (!paths.has_value()) return std::nullopt;
        std::vector<std::string> out;
        for (std::size_t at = 0; at < paths->size(); ++at) {
            const std::string& source = (*paths)[at];
            const bool blocked = at < attachment.image_artifacts.size() &&
                                 attachment.image_artifacts[at].unavailable;
            std::optional<std::string> resolved;
            if (not_blank(source) && !blocked) resolved = rewrite_path(source);
            out.push_back(resolved.value_or(""));
            if (json::is_blank(out.back())) on_missing();
        }
        return out;
    };

    const std::optional<std::vector<std::string>> frames = rewrite(attachment.processed_frames);
    const std::optional<std::vector<std::string>> pages = rewrite(attachment.pre_rendered_paths);

    std::vector<std::string> targets;
    if (attachment.type == "image") {
        targets.push_back(primary.has_value() ? primary->first : std::string());
    } else if (attachment.type == "video") {
        targets = frames.value_or(std::vector<std::string>{});
    } else if (attachment.type == "pdf") {
        targets = pages.value_or(std::vector<std::string>{});
    }

    const std::size_t count = std::max(targets.size(), attachment.image_artifacts.size());
    std::vector<ToolImageAttachment> artifacts;
    for (std::size_t at = 0; at < count; ++at) {
        const ToolImageAttachment recorded =
            at < attachment.image_artifacts.size()
                ? attachment.image_artifacts[at]
                : unmeasured_artifact(attachment.mime_type,
                                      targets.size() == 1 ? attachment.pixel_width : std::nullopt,
                                      targets.size() == 1 ? attachment.pixel_height : std::nullopt);
        const std::optional<std::string> target =
            at < targets.size() ? std::optional<std::string>(targets[at]) : std::nullopt;
        if (recorded.unavailable || !target.has_value() || json::is_blank(*target)) {
            artifacts.push_back(as_unavailable_placeholder(recorded));
        } else {
            ToolImageAttachment rewritten = recorded;
            rewritten.path = *target;
            artifacts.push_back(std::move(rewritten));
        }
    }

    const bool media_type =
        attachment.type == "image" || attachment.type == "pdf" || attachment.type == "video";
    bool any_unavailable = false;
    for (const ToolImageAttachment& artifact : artifacts) {
        any_unavailable = any_unavailable || artifact.unavailable;
    }

    SelectedAttachment out = attachment;
    out.uri = primary.has_value() ? primary->second : std::string();
    out.local_path = primary.has_value() ? std::optional<std::string>(primary->first) : std::nullopt;
    out.processed_frames = frames;
    out.pre_rendered_paths = pages;
    out.image_artifacts = std::move(artifacts);
    out.storage = AttachmentStorage::app_private;
    out.sandbox_path.reset();
    out.unavailable = attachment.unavailable || !primary.has_value() ||
                      (media_type && any_unavailable);
    return out;
}

/** Strict kotlinx decode: a malformed column throws JsonDecodingException instead of falling back. */
/**
 * kotlinx decodeFromString<List<MessageSegment>> reports the first offending element like its
 * lexer does: `Unexpected JSON token at offset N: Expected start of the object '{', but had 'c'
 * instead at path: $[k]\nJSON input: <raw>`. Only that shape (and the raw input itself) is
 * observable in the pinned scenarios, so the message is built for it directly.
 */
[[noreturn]] void throw_segments_decode_error(const std::string& raw, const std::string& fallback) {
    const std::size_t start = raw.find_first_not_of(" \t\r\n");
    if (start != std::string::npos && raw[start] == '[') {
        std::size_t at = start + 1;
        std::size_t element = 0;
        while (at < raw.size()) {
            while (at < raw.size() && (raw[at] == ' ' || raw[at] == '\t' || raw[at] == '\r' ||
                                       raw[at] == '\n' || raw[at] == ',')) {
                if (raw[at] == ',') element += 1;
                ++at;
            }
            if (at >= raw.size()) break;
            if (raw[at] != '{') {
                throw KotlinError(
                    "JsonDecodingException",
                    "Unexpected JSON token at offset " + std::to_string(at) +
                        ": Expected start of the object '{', but had '" +
                        std::string(1, raw[at]) + "' instead at path: $[" +
                        std::to_string(element) + "]\nJSON input: " + raw);
            }
            // A well-formed element is skipped to reach the next failing one.
            std::size_t depth = 0;
            bool in_string = false;
            for (; at < raw.size(); ++at) {
                const char c = raw[at];
                if (in_string) {
                    if (c == '\\') ++at;
                    else if (c == '"') in_string = false;
                    continue;
                }
                if (c == '"') in_string = true;
                else if (c == '{') depth += 1;
                else if (c == '}') {
                    depth -= 1;
                    if (depth == 0) { ++at; break; }
                }
            }
        }
    }
    throw KotlinError("JsonDecodingException", fallback);
}

std::vector<MessageSegment> strict_message_segments(const std::string& raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != Kind::Array) {
        throw_segments_decode_error(raw, "message segments are not a JSON array");
    }
    try {
        std::vector<MessageSegment> segments;
        for (const json::Value& element : parsed.value->elements) {
            segments.push_back(decode_message_segment(element));
        }
        return segments;
    } catch (const DecodeError&) {
        throw_segments_decode_error(raw, "a message segment failed to decode");
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------------------------

std::string encode_attachment_item(const AttachmentItem& item) {
    JsonOut out;
    out.open_object();
    put_string(out, "originalUri", item.original_uri);
    out.string_field("type", item.type);
    put_string(out, "file_name", item.file_name);
    put_string(out, "mime_type", item.mime_type);
    put_int(out, "image_index", item.image_index);
    put_int(out, "page_count", item.page_count);
    put_int(out, "pixel_width", item.pixel_width);
    put_int(out, "pixel_height", item.pixel_height);
    put_string(out, "warning", item.warning);
    put_string(out, "text_content", item.text_content);
    put_string(out, "transcription", item.transcription);
    put_storage(out, "storage", item.storage);
    put_string(out, "sandbox_path", item.sandbox_path);
    put_long(out, "file_size", item.file_size);
    out.bool_field("unavailable", item.unavailable, false);
    if (!item.image_artifacts.empty()) {
        out.key("imageArtifacts");
        out.open_array();
        for (const ToolImageAttachment& artifact : item.image_artifacts) {
            out.raw_value(encode_tool_image(artifact));
        }
        out.close_array();
    }
    out.close_object();
    return out.take();
}

std::string encode_attachment_meta(const AttachmentMeta& meta) {
    JsonOut out;
    out.open_object();
    if (!meta.items.empty()) {
        out.key("items");
        out.open_array();
        for (const AttachmentItem& item : meta.items) out.raw_value(encode_attachment_item(item));
        out.close_array();
    }
    out.close_object();
    return out.take();
}

std::string encode_tool_image(const ToolImageAttachment& image) {
    JsonOut out;
    out.open_object();
    out.string_field("path", image.path);
    out.string_field("mimeType", image.mime_type);
    out.number_field("sizeBytes", image.size_bytes);
    put_int(out, "width", image.width);
    put_int(out, "height", image.height);
    if (image.policy_version != 0) out.number_field("policyVersion", image.policy_version);
    out.bool_field("unavailable", image.unavailable, false);
    out.close_object();
    return out.take();
}

std::string encode_selected_attachment(const SelectedAttachment& attachment) {
    JsonOut out;
    out.open_object();
    out.string_field("localId", attachment.local_id);
    out.string_field("uri", attachment.uri);
    out.string_field("type", attachment.type);
    put_int(out, "frameCount", attachment.frame_count);
    put_long(out, "sliceIntervalMs", attachment.slice_interval_ms);
    put_int(out, "pageCount", attachment.page_count);
    put_long(out, "videoDurationMs", attachment.video_duration_ms);
    put_string(out, "fileName", attachment.file_name);
    put_string(out, "mimeType", attachment.mime_type);
    put_long(out, "fileSize", attachment.file_size);
    put_int(out, "pixelWidth", attachment.pixel_width);
    put_int(out, "pixelHeight", attachment.pixel_height);
    put_string_list(out, "processedFrames", attachment.processed_frames);
    if (attachment.selected_pages.has_value()) {  // default is null, so an empty set still encodes
        out.key("selectedPages");
        out.open_array();
        for (int page : *attachment.selected_pages) out.number_value(page);
        out.close_array();
    }
    put_string_list(out, "preRenderedPaths", attachment.pre_rendered_paths);
    put_string(out, "localPath", attachment.local_path);
    put_storage(out, "storage", attachment.storage);
    put_string(out, "sandboxPath", attachment.sandbox_path);
    put_import_state(out, "importState", attachment.import_state);
    put_string(out, "preparedText", attachment.prepared_text);
    out.bool_field("unavailable", attachment.unavailable, false);
    if (!attachment.image_artifacts.empty()) {
        out.key("imageArtifacts");
        out.open_array();
        for (const ToolImageAttachment& artifact : attachment.image_artifacts) {
            out.raw_value(encode_tool_image(artifact));
        }
        out.close_array();
    }
    out.close_object();
    return out.take();
}

std::string encode_message_segment(const MessageSegment& segment) {
    JsonOut out;
    out.open_object();
    out.string_field("type", segment.type);
    if (!segment.content.empty()) out.string_field("content", segment.content);
    put_string(out, "toolName", segment.tool_name);
    put_string(out, "toolArgs", segment.tool_args);
    put_string(out, "toolResult", segment.tool_result);
    put_string(out, "toolCallId", segment.tool_call_id);
    put_string(out, "signature", segment.signature);
    put_string(out, "signatureProvider", segment.signature_provider);
    put_long(out, "durationMs", segment.duration_ms);
    put_string(out, "toolState", segment.tool_state);
    put_string(out, "toolProgress", segment.tool_progress);
    put_string(out, "toolTarget", segment.tool_target);
    put_string(out, "toolDisplayName", segment.tool_display_name);
    put_string(out, "toolResultText", segment.tool_result_text);
    put_string(out, "toolStructuredResult", segment.tool_structured_result);
    if (!segment.tool_images.empty()) {
        out.key("toolImages");
        out.open_array();
        for (const ToolImageAttachment& image : segment.tool_images) {
            out.raw_value(encode_tool_image(image));
        }
        out.close_array();
    }
    put_string(out, "toolTranscription", segment.tool_transcription);
    put_raw_list(out, "responseOutputItems", segment.response_output_items);
    put_string(out, "responseOutputItemProvider", segment.response_output_item_provider);
    put_string(out, "errorCode", segment.error_code);
    out.close_object();
    return out.take();
}

// ---------------------------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------------------------

std::optional<AttachmentMeta> decode_attachment_meta(std::string_view raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != Kind::Object) return std::nullopt;
    const json::Value& object = *parsed.value;
    AttachmentMeta meta;
    try {
        for (const json::Value& element : read_array(object, "items")) {
            meta.items.push_back(decode_attachment_item(element));
        }
    } catch (const DecodeError&) {
        return std::nullopt;
    }
    return meta;
}

std::optional<std::vector<MessageSegment>> decode_message_segments(std::string_view raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != Kind::Array) return std::nullopt;
    try {
        std::vector<MessageSegment> segments;
        for (const json::Value& element : parsed.value->elements) {
            segments.push_back(decode_message_segment(element));
        }
        return segments;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

namespace {

std::string encode_segments(const std::vector<MessageSegment>& segments) {
    JsonOut out;
    out.open_array();
    for (const MessageSegment& segment : segments) out.raw_value(encode_message_segment(segment));
    out.close_array();
    return out.take();
}

}  // namespace

std::string encode_message_segments(const std::vector<MessageSegment>& segments) {
    return encode_segments(segments);
}

// ---------------------------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------------------------

std::vector<SelectedAttachment> NativeBackupMediaPolicy::exportable_draft_attachments(
    const std::vector<SelectedAttachment>& attachments) {
    std::vector<SelectedAttachment> kept;
    for (const SelectedAttachment& attachment : attachments) {
        if (!is_local_sandbox(attachment.storage)) kept.push_back(attachment);
    }
    return kept;
}

std::vector<SelectedAttachment> NativeBackupMediaPolicy::rewrite_draft_attachments_for_export(
    const std::vector<SelectedAttachment>& attachments, const SourceLookup& archive_entry,
    const MissingResource& on_missing) {
    std::vector<SelectedAttachment> result;
    for (const SelectedAttachment& attachment : exportable_draft_attachments(attachments)) {
        std::optional<std::string> file_name = attachment.file_name;
        if (!file_name.has_value()) {
            for (const std::string& source : file_name_candidates(attachment)) {
                const std::optional<std::string> candidate = source_file_name(source);
                if (candidate.has_value()) {
                    file_name = candidate;
                    break;
                }
            }
        }

        std::optional<std::string> primary_source;
        for (const std::string& source : primary_candidates(attachment)) {
            const std::optional<std::string> entry = archive_entry(source);
            if (entry.has_value()) {
                primary_source = entry;
                break;
            }
        }
        if (!primary_source.has_value()) on_missing();

        std::optional<std::pair<std::string, std::string>> primary;
        if (primary_source.has_value()) {
            primary = std::make_pair(*primary_source, *primary_source);
        }
        SelectedAttachment rewritten =
            rewrite_draft_media(attachment, primary, archive_entry, on_missing);
        rewritten.file_name = file_name;
        result.push_back(std::move(rewritten));
    }
    return result;
}

std::vector<SelectedAttachment> NativeBackupMediaPolicy::restore_draft_attachments(
    const std::vector<SelectedAttachment>& attachments, const PrimaryLookup& restored_primary,
    const PathLookup& restored_path) {
    std::vector<SelectedAttachment> result;
    for (const SelectedAttachment& attachment : attachments) {
        std::optional<std::string> primary_entry;
        if (!null_or_blank(attachment.local_path)) {
            primary_entry = attachment.local_path;
        } else if (!null_or_blank(attachment.uri)) {
            primary_entry = attachment.uri;
        }
        std::optional<std::pair<std::string, std::string>> primary;
        if (primary_entry.has_value()) primary = restored_primary(*primary_entry);

        SelectedAttachment rewritten =
            rewrite_draft_media(attachment, primary, restored_path, []() {});
        if (!rewritten.file_name.has_value() && primary_entry.has_value()) {
            rewritten.file_name = source_file_name(*primary_entry);
        }
        result.push_back(std::move(rewritten));
    }
    return result;
}

std::vector<std::string> NativeBackupMediaPolicy::tool_image_paths(
    const std::optional<std::string>& raw) {
    if (null_or_blank(raw)) return {};
    const std::optional<std::vector<MessageSegment>> segments = decode_message_segments(*raw);
    if (!segments.has_value()) return {};
    std::vector<std::string> paths;
    for (const MessageSegment& segment : *segments) {
        for (const ToolImageAttachment& image : segment.tool_images) {
            if (image.unavailable) continue;
            if (not_blank(image.path)) paths.push_back(image.path);
        }
    }
    return paths;
}

std::optional<std::string> NativeBackupMediaPolicy::rewrite_attachment_meta_for_export(
    const std::optional<std::string>& raw, const std::vector<std::string>& original_image_sources,
    const std::map<int, int>& old_to_new_image_index, const SourceLookup& archive_entry,
    const MissingResource& on_missing) {
    std::map<int, std::string> indexed_sources;
    for (std::size_t at = 0; at < original_image_sources.size(); ++at) {
        indexed_sources[static_cast<int>(at)] = original_image_sources[at];
    }
    const std::vector<AttachmentItem> source_items = attachment_items(raw, indexed_sources);
    if (source_items.empty()) return std::nullopt;

    std::vector<AttachmentItem> items;
    for (const AttachmentItem& item : source_items) {
        if (is_local_sandbox(item.storage)) continue;

        const std::vector<ToolImageAttachment> artifacts = rewrite_message_artifacts(
            item, old_to_new_image_index, indexed_sources, archive_entry, false);
        std::optional<std::string> archived_original_uri;
        if (item.original_uri.has_value() && !item.unavailable && item.type == "video") {
            archived_original_uri = archive_entry(*item.original_uri);
        }

        bool any_unavailable = false;
        for (const ToolImageAttachment& artifact : artifacts) {
            any_unavailable = any_unavailable || artifact.unavailable;
        }
        bool unavailable = false;
        if (item.unavailable) {
            unavailable = true;
        } else if (item.type == "video") {
            unavailable = !archived_original_uri.has_value();
        } else if (item.type == "file") {
            unavailable = !item.text_content.has_value();
        } else if (item.type == "image" || item.type == "pdf") {
            unavailable = artifacts.empty() || any_unavailable;
        }
        if (unavailable) on_missing();

        AttachmentItem rewritten = item;
        rewritten.original_uri = archived_original_uri;
        if (!item.file_name.has_value() && item.original_uri.has_value()) {
            rewritten.file_name = source_file_name(*item.original_uri);
        }
        if (item.image_index.has_value()) {
            const auto found = old_to_new_image_index.find(*item.image_index);
            rewritten.image_index =
                found == old_to_new_image_index.end() ? std::nullopt : std::optional<int>(found->second);
        } else {
            rewritten.image_index = std::nullopt;
        }
        rewritten.image_artifacts = artifacts;
        if (unavailable) rewritten.text_content.reset();
        rewritten.unavailable = unavailable;
        items.push_back(std::move(rewritten));
    }
    return encode_attachment_meta(AttachmentMeta{std::move(items)});
}

std::optional<std::string> NativeBackupMediaPolicy::rewrite_tool_image_paths_for_export(
    const std::optional<std::string>& raw, const SourceLookup& archive_entry,
    const MissingResource& on_missing) {
    if (null_or_blank(raw)) return raw;
    std::vector<MessageSegment> segments = strict_message_segments(*raw);
    for (MessageSegment& segment : segments) {
        std::vector<ToolImageAttachment> rewritten_images;
        for (const ToolImageAttachment& image : segment.tool_images) {
            std::optional<std::string> archived;
            if (!image.unavailable && not_blank(image.path)) archived = archive_entry(image.path);
            if (!archived.has_value()) {
                on_missing();
                rewritten_images.push_back(as_unavailable_placeholder(image));
            } else {
                ToolImageAttachment rewritten = image;
                rewritten.path = *archived;
                rewritten_images.push_back(std::move(rewritten));
            }
        }
        segment.tool_images = std::move(rewritten_images);
    }
    return encode_segments(segments);
}

std::optional<std::string> NativeBackupMediaPolicy::restore_attachment_meta(
    const std::optional<std::string>& raw, int archive_version,
    const std::map<int, int>& old_to_new_image_index,
    const std::map<int, std::string>& legacy_video_uris, const PathLookup& restored_uri,
    const PathLookup& restored_path, const std::vector<std::string>& original_image_sources,
    const std::map<int, std::string>& legacy_image_paths) {
    // attachmentItems(raw, indexedSources): below v4 the legacy path map is the source table.
    std::map<int, std::string> indexed_sources;
    if (archive_version < 4) {
        indexed_sources = legacy_image_paths;
    } else {
        for (std::size_t at = 0; at < original_image_sources.size(); ++at) {
            indexed_sources[static_cast<int>(at)] = original_image_sources[at];
        }
    }
    const std::vector<AttachmentItem> source_items = attachment_items(raw, indexed_sources);
    if (source_items.empty()) return std::nullopt;

    std::vector<AttachmentItem> items;
    for (const AttachmentItem& item : source_items) {
        const std::vector<ToolImageAttachment> artifacts = rewrite_message_artifacts(
            item, old_to_new_image_index, indexed_sources, restored_path, archive_version < 4);
        std::optional<std::string> restored_original_uri;
        if (item.unavailable) {
            restored_original_uri.reset();
        } else if (archive_version >= 4) {
            if (item.original_uri.has_value()) restored_original_uri = restored_uri(*item.original_uri);
        } else if (item.type == "video") {
            const auto legacy = legacy_video_uris.find(item.image_index.value_or(0));
            if (legacy != legacy_video_uris.end()) restored_original_uri = legacy->second;
        }

        bool any_unavailable = false;
        for (const ToolImageAttachment& artifact : artifacts) {
            any_unavailable = any_unavailable || artifact.unavailable;
        }
        bool unavailable = false;
        if (item.unavailable) {
            unavailable = true;
        } else if (item.type == "video") {
            unavailable = !restored_original_uri.has_value();
        } else if (item.type == "file") {
            unavailable = !item.text_content.has_value();
        } else if (item.type == "image" || item.type == "pdf") {
            unavailable = artifacts.empty() || any_unavailable;
        }

        AttachmentItem rewritten = item;
        rewritten.original_uri = restored_original_uri;
        if (!item.file_name.has_value() && item.original_uri.has_value()) {
            rewritten.file_name = source_file_name(*item.original_uri);
        }
        if (item.image_index.has_value()) {
            const auto found = old_to_new_image_index.find(*item.image_index);
            rewritten.image_index =
                found == old_to_new_image_index.end() ? std::nullopt : std::optional<int>(found->second);
        } else {
            rewritten.image_index = std::nullopt;
        }
        rewritten.image_artifacts = artifacts;
        if (unavailable) rewritten.text_content.reset();
        rewritten.storage = AttachmentStorage::app_private;
        rewritten.sandbox_path.reset();
        rewritten.unavailable = unavailable;
        items.push_back(std::move(rewritten));
    }
    return encode_attachment_meta(AttachmentMeta{std::move(items)});
}

std::optional<std::string> NativeBackupMediaPolicy::restore_tool_image_paths(
    const std::optional<std::string>& raw, int archive_version, const PathLookup& restored_path) {
    if (null_or_blank(raw)) return raw;
    std::vector<MessageSegment> segments = strict_message_segments(*raw);
    for (MessageSegment& segment : segments) {
        std::vector<ToolImageAttachment> rewritten_images;
        for (const ToolImageAttachment& image : segment.tool_images) {
            std::optional<std::string> restored;
            if (archive_version >= 4 && !image.unavailable && not_blank(image.path)) {
                restored = restored_path(image.path);
            }
            if (!restored.has_value()) {
                rewritten_images.push_back(as_unavailable_placeholder(image));
            } else {
                ToolImageAttachment rewritten = image;
                rewritten.path = *restored;
                rewritten_images.push_back(std::move(rewritten));
            }
        }
        segment.tool_images = std::move(rewritten_images);
    }
    return encode_segments(segments);
}

}  // namespace agora::core::backup
