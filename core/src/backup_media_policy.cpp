#include "agora/core/backup_media_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>

#include "agora/core/json.hpp"

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

AttachmentImportState read_import_state(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr) return AttachmentImportState::ready;
    if (is_json_null(*value) || value->kind != Kind::String) decode_fail();
    if (value->text == "processing") return AttachmentImportState::processing;
    if (value->text == "ready") return AttachmentImportState::ready;
    if (value->text == "failed") return AttachmentImportState::failed;
    decode_fail();
}

std::optional<std::vector<std::string>> read_string_list(const json::Value& object,
                                                         std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value)) return std::nullopt;
    if (value->kind != Kind::Array) decode_fail();
    std::vector<std::string> out;
    for (const json::Value& element : value->elements) {
        if (element.kind != Kind::String) decode_fail();
        out.push_back(element.text);
    }
    return out;
}

std::optional<std::vector<int>> read_int_set(const json::Value& object, std::string_view name) {
    const json::Value* value = lookup(object, name);
    if (value == nullptr || is_json_null(*value)) return std::nullopt;
    if (value->kind != Kind::Array) decode_fail();
    std::vector<int> out;
    for (const json::Value& element : value->elements) {
        if (element.kind != Kind::Literal) decode_fail();
        std::int64_t parsed = 0;
        if (!parse_int_literal(element.text, parsed) || parsed < -2147483648ll ||
            parsed > 2147483647ll) {
            decode_fail();
        }
        out.push_back(static_cast<int>(parsed));
    }
    return out;
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
    image.sha256 = read_required_string(value, "sha256");
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

/**
 * The candidate list behind listOfNotNull(localPath, uri.takeIf(::isNotBlank)):
 * a present-but-blank localPath is still a candidate, which the golden call log pins.
 */
std::vector<std::string> candidate_sources(const SelectedAttachment& attachment) {
    std::vector<std::string> sources;
    if (attachment.local_path.has_value()) sources.push_back(*attachment.local_path);
    if (!null_or_blank(attachment.uri)) sources.push_back(attachment.uri);
    return sources;
}

/** firstNotNullOfOrNull(::sourceFileName) over the candidate list. */
std::optional<std::string> derived_file_name(const SelectedAttachment& attachment) {
    for (const std::string& source : candidate_sources(attachment)) {
        const std::optional<std::string> name = source_file_name(source);
        if (name.has_value()) return name;
    }
    return std::nullopt;
}

std::optional<int> first_or_null(const std::vector<int>& values) {
    return values.empty() ? std::nullopt : std::optional<int>(values.front());
}

/** firstNotNullOfOrNull(archiveEntryForSource) over the candidate list. */
std::optional<std::string> first_archived(const std::vector<std::string>& sources,
                                          const SourceLookup& lookup_fn) {
    for (const std::string& source : sources) {
        const std::optional<std::string> entry = lookup_fn(source);
        if (entry.has_value()) return entry;
    }
    return std::nullopt;
}

/** ?.mapNotNull(f)?.takeIf(List::isNotEmpty) */
template <class Resolve>
std::optional<std::vector<std::string>> map_not_null_take_if(
    const std::optional<std::vector<std::string>>& values, Resolve resolve) {
    if (!values.has_value()) return std::nullopt;
    std::vector<std::string> kept;
    for (const std::string& value : *values) {
        const std::optional<std::string> resolved = resolve(value);
        if (resolved.has_value()) kept.push_back(*resolved);
    }
    return kept.empty() ? std::nullopt : std::optional<std::vector<std::string>>(kept);
}

int coerced_count(const std::optional<int>& page_count) {
    if (!page_count.has_value()) return 1;
    return std::max(1, *page_count);
}

/** (start until start + count).mapNotNull(oldToNewImageIndex::get) */
std::vector<int> surviving_indices(const std::optional<int>& image_index, int count,
                                   const std::map<int, int>& remap) {
    std::vector<int> surviving;
    if (!image_index.has_value()) return surviving;
    for (int offset = 0; offset < count; ++offset) {
        const auto found = remap.find(*image_index + offset);
        if (found != remap.end()) surviving.push_back(found->second);
    }
    return surviving;
}

SelectedAttachment as_unavailable_placeholder(const SelectedAttachment& attachment,
                                               const std::optional<std::string>& retained_name) {
    SelectedAttachment out = attachment;
    out.uri.clear();
    out.file_name = retained_name;
    out.processed_frames.reset();
    out.selected_pages.reset();
    out.pre_rendered_paths.reset();
    out.local_path.reset();
    out.storage = AttachmentStorage::app_private;
    out.sandbox_path.reset();
    out.unavailable = true;
    return out;
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
    out.string_field("sha256", image.sha256);
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
        if (!file_name.has_value()) file_name = derived_file_name(attachment);

        if (attachment.unavailable) {
            on_missing();
            result.push_back(as_unavailable_placeholder(attachment, file_name));
            continue;
        }

        const std::optional<std::string> primary =
            first_archived(candidate_sources(attachment), archive_entry);
        if (!primary.has_value()) {
            on_missing();
            result.push_back(as_unavailable_placeholder(attachment, file_name));
            continue;
        }

        SelectedAttachment rewritten = attachment;
        rewritten.uri = *primary;
        rewritten.local_path = *primary;
        rewritten.file_name = file_name;
        rewritten.processed_frames =
            map_not_null_take_if(attachment.processed_frames, [&](const std::string& path) {
                return archive_entry(path);
            });
        rewritten.pre_rendered_paths =
            map_not_null_take_if(attachment.pre_rendered_paths, [&](const std::string& path) {
                return archive_entry(path);
            });
        rewritten.storage = AttachmentStorage::app_private;
        rewritten.sandbox_path.reset();
        rewritten.unavailable = false;
        result.push_back(rewritten);
    }
    return result;
}

std::vector<SelectedAttachment> NativeBackupMediaPolicy::restore_draft_attachments(
    const std::vector<SelectedAttachment>& attachments, const PrimaryLookup& restored_primary,
    const PathLookup& restored_path) {
    std::vector<SelectedAttachment> result;
    for (const SelectedAttachment& attachment : attachments) {
        if (attachment.unavailable) {
            result.push_back(as_unavailable_placeholder(attachment, attachment.file_name));
            continue;
        }

        std::optional<std::string> primary_entry = attachment.local_path;
        if (null_or_blank(primary_entry)) {
            primary_entry = json::is_blank(attachment.uri)
                                ? std::nullopt
                                : std::optional<std::string>(attachment.uri);
        }
        const std::optional<std::pair<std::string, std::string>> primary =
            primary_entry.has_value() ? restored_primary(*primary_entry) : std::nullopt;

        if (!primary.has_value()) {
            std::optional<std::string> retained = attachment.file_name;
            if (!retained.has_value() && primary_entry.has_value()) {
                retained = source_file_name(*primary_entry);
            }
            result.push_back(as_unavailable_placeholder(attachment, retained));
            continue;
        }

        SelectedAttachment rewritten = attachment;
        rewritten.uri = primary->second;
        rewritten.local_path = primary->first;
        rewritten.processed_frames = map_not_null_take_if(
            attachment.processed_frames, [&](const std::string& path) { return restored_path(path); });
        rewritten.pre_rendered_paths = map_not_null_take_if(
            attachment.pre_rendered_paths, [&](const std::string& path) { return restored_path(path); });
        rewritten.storage = AttachmentStorage::app_private;
        rewritten.sandbox_path.reset();
        rewritten.unavailable = false;
        result.push_back(rewritten);
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
        for (const ToolImageAttachment& image : segment.tool_images) paths.push_back(image.path);
    }
    return paths;
}

std::optional<std::string> NativeBackupMediaPolicy::rewrite_attachment_meta_for_export(
    const std::optional<std::string>& raw, const std::vector<std::string>& original_image_sources,
    const std::map<int, int>& old_to_new_image_index, const SourceLookup& archive_entry,
    const MissingResource& on_missing) {
    std::optional<AttachmentMeta> decoded;
    if (raw.has_value()) decoded = decode_attachment_meta(*raw);

    // Image indices the decoded meta already accounts for; legacy sources must not duplicate them.
    std::set<int> claimed;
    if (decoded.has_value()) {
        for (const AttachmentItem& item : decoded->items) {
            if (!item.image_index.has_value()) continue;
            const int count = coerced_count(item.page_count);
            for (int offset = 0; offset < count; ++offset) claimed.insert(*item.image_index + offset);
        }
    }

    std::vector<AttachmentItem> source_items;
    if (decoded.has_value()) source_items = decoded->items;
    for (std::size_t index = 0; index < original_image_sources.size(); ++index) {
        if (claimed.count(static_cast<int>(index)) != 0) continue;
        AttachmentItem legacy;
        legacy.original_uri = original_image_sources[index];
        legacy.type = "image";
        legacy.file_name = source_file_name(original_image_sources[index]);
        legacy.image_index = static_cast<int>(index);
        source_items.push_back(legacy);
    }

    if (source_items.empty()) return std::nullopt;

    std::vector<AttachmentItem> items;
    for (const AttachmentItem& item : source_items) {
        if (is_local_sandbox(item.storage)) continue;
        const int original_count = coerced_count(item.page_count);
        const std::vector<int> surviving =
            surviving_indices(item.image_index, original_count, old_to_new_image_index);

        std::optional<std::string> archived_original_uri;
        if (item.original_uri.has_value() && !item.unavailable && item.type == "video") {
            archived_original_uri = archive_entry(*item.original_uri);
        }

        bool unavailable = false;
        if (item.unavailable) {
            unavailable = true;
        } else if (item.type == "video") {
            unavailable = !archived_original_uri.has_value();
        } else if (item.type == "file") {
            unavailable = !item.text_content.has_value();
        } else if (item.type == "image" || item.type == "pdf") {
            unavailable = surviving.empty();
        }
        if (unavailable) on_missing();

        AttachmentItem rewritten = item;
        rewritten.original_uri = archived_original_uri;
        if (!item.file_name.has_value() && item.original_uri.has_value()) {
            rewritten.file_name = source_file_name(*item.original_uri);
        }
        rewritten.image_index =
            (unavailable || !item.image_index.has_value()) ? std::nullopt : first_or_null(surviving);
        rewritten.page_count = (unavailable || !item.page_count.has_value())
                                   ? std::nullopt
                                   : std::optional<int>(static_cast<int>(surviving.size()));
        if (unavailable) rewritten.text_content.reset();
        rewritten.unavailable = unavailable;
        items.push_back(rewritten);
    }
    return encode_attachment_meta(AttachmentMeta{std::move(items)});
}

std::optional<std::string> NativeBackupMediaPolicy::rewrite_tool_image_paths_for_export(
    const std::optional<std::string>& raw, const SourceLookup& archive_entry) {
    if (null_or_blank(raw)) return raw;
    const std::optional<std::vector<MessageSegment>> decoded = decode_message_segments(*raw);
    if (!decoded.has_value()) return raw;

    std::vector<MessageSegment> segments = *decoded;
    for (MessageSegment& segment : segments) {
        std::vector<ToolImageAttachment> kept;
        for (const ToolImageAttachment& image : segment.tool_images) {
            const std::optional<std::string> archived = archive_entry(image.path);
            if (archived.has_value()) {
                ToolImageAttachment rewritten = image;
                rewritten.path = *archived;
                kept.push_back(rewritten);
            }
        }
        segment.tool_images = std::move(kept);
    }
    return encode_segments(segments);
}

std::optional<std::string> NativeBackupMediaPolicy::restore_attachment_meta(
    const std::optional<std::string>& raw, int archive_version,
    const std::map<int, int>& old_to_new_image_index,
    const std::map<int, std::string>& legacy_video_uris, const PathLookup& restored_uri) {
    if (null_or_blank(raw)) return raw;
    const std::optional<AttachmentMeta> decoded = decode_attachment_meta(*raw);
    if (!decoded.has_value()) return std::nullopt;

    std::vector<AttachmentItem> items;
    for (const AttachmentItem& item : decoded->items) {
        const int original_count = coerced_count(item.page_count);
        const std::vector<int> surviving =
            surviving_indices(item.image_index, original_count, old_to_new_image_index);

        std::optional<std::string> restored_original_uri;
        if (item.unavailable) {
            restored_original_uri.reset();
        } else if (archive_version >= 4) {
            if (item.original_uri.has_value()) restored_original_uri = restored_uri(*item.original_uri);
        } else if (item.type == "video") {
            const auto legacy = legacy_video_uris.find(item.image_index.value_or(0));
            if (legacy != legacy_video_uris.end()) restored_original_uri = legacy->second;
        }

        bool unavailable = false;
        if (item.unavailable) {
            unavailable = true;
        } else if (item.type == "video") {
            unavailable = !restored_original_uri.has_value();
        } else if (item.type == "file") {
            unavailable = !item.text_content.has_value();
        } else if (item.type == "image" || item.type == "pdf") {
            unavailable = surviving.empty();
        }

        AttachmentItem rewritten = item;
        rewritten.original_uri = restored_original_uri;
        rewritten.image_index =
            (unavailable || !item.image_index.has_value()) ? std::nullopt : first_or_null(surviving);
        rewritten.page_count = (unavailable || !item.page_count.has_value())
                                   ? std::nullopt
                                   : std::optional<int>(static_cast<int>(surviving.size()));
        if (unavailable) rewritten.text_content.reset();
        rewritten.storage = AttachmentStorage::app_private;
        rewritten.sandbox_path.reset();
        rewritten.unavailable = unavailable;
        items.push_back(rewritten);
    }
    return encode_attachment_meta(AttachmentMeta{std::move(items)});
}

std::optional<std::string> NativeBackupMediaPolicy::restore_tool_image_paths(
    const std::optional<std::string>& raw, int archive_version, const PathLookup& restored_path) {
    if (null_or_blank(raw)) return raw;
    const std::optional<std::vector<MessageSegment>> decoded = decode_message_segments(*raw);
    if (!decoded.has_value()) return raw;

    std::vector<MessageSegment> segments = *decoded;
    for (MessageSegment& segment : segments) {
        std::vector<ToolImageAttachment> kept;
        for (const ToolImageAttachment& image : segment.tool_images) {
            if (archive_version < 4) {
                // v1-v3 stored only a device-absolute path. Dropping an unusable thumbnail is
                // safer than retaining a misleading foreign path.
                continue;
            }
            const std::optional<std::string> restored = restored_path(image.path);
            if (restored.has_value()) {
                ToolImageAttachment rewritten = image;
                rewritten.path = *restored;
                kept.push_back(rewritten);
            }
        }
        segment.tool_images = std::move(kept);
    }
    return encode_segments(segments);
}

}  // namespace agora::core::backup