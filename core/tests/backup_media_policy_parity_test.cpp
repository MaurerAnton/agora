// Golden parity test for the native NativeBackupMediaPolicy port.
//
// Replays every scenario under core/tools/golden/backup-media-policy/scenarios through
// agora::core::backup::NativeBackupMediaPolicy and requires the produced trace to equal the frozen
// Kotlin trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/backup-media-policy/BackupMediaTraceDump.kt).
//
// The CALL LOG is the point of most of these scenarios. The Kotlin policy resolves its candidate
// sources with firstNotNullOfOrNull, which stops at the first hit, so a port that probes in a
// different order can return the same value while asking the archive layer for different paths --
// a real bug in a backup format that no amount of result-only testing would catch.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace backup = agora::core::backup;
using json::Kind;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** The stand's trace escaping, identical to esc() in the reference driver. */
std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

/** `-` stands for an absent result, exactly as the driver writes it. */
std::string opt(const std::optional<std::string>& value) {
    return value.has_value() ? esc(*value) : "-";
}

/** Re-quotes a bare string the way kotlinx's encoder would. */
std::string quote_json_string(std::string_view text) {
    json::Value value;
    value.kind = Kind::String;
    value.text = std::string(text);
    return json::to_kotlin_string(value);
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

bool is_json_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::optional<std::string> string_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_json_null(value)) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_field(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = string_field(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::optional<std::int64_t> int_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_json_null(value)) return std::nullopt;
    return std::stoll(value->text);
}

std::optional<int> optional_int(const json::Value& object, std::string_view name) {
    const std::optional<std::int64_t> value = int_field(object, name);
    return value.has_value() ? std::optional<int>(static_cast<int>(*value)) : std::nullopt;
}

int int_field_or(const json::Value& object, std::string_view name, int fallback) {
    const std::optional<std::int64_t> value = int_field(object, name);
    return value.has_value() ? static_cast<int>(*value) : fallback;
}

std::optional<std::vector<std::string>> string_list_field(const json::Value& object,
                                                          std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_json_null(value)) return std::nullopt;
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    std::vector<std::string> out;
    for (const json::Value& element : value->elements) out.push_back(element.text);
    return out;
}

backup::AttachmentStorage storage_of(const std::optional<std::string>& name) {
    const std::string value = name.value_or("app_private");
    if (value == "app_private") return backup::AttachmentStorage::app_private;
    if (value == "local_sandbox_pending") return backup::AttachmentStorage::local_sandbox_pending;
    if (value == "local_sandbox_runtime") return backup::AttachmentStorage::local_sandbox_runtime;
    scenario_fail("unknown attachment storage " + value);
}

backup::AttachmentImportState import_state_of(const std::optional<std::string>& name) {
    const std::string value = name.value_or("ready");
    if (value == "processing") return backup::AttachmentImportState::processing;
    if (value == "ready") return backup::AttachmentImportState::ready;
    if (value == "failed") return backup::AttachmentImportState::failed;
    scenario_fail("unknown import state " + value);
}

backup::SelectedAttachment decode_attachment(const json::Value& value) {
    backup::SelectedAttachment attachment;
    attachment.local_id = required_field(value, "localId");
    attachment.uri = required_field(value, "uri");
    attachment.type = required_field(value, "type");
    attachment.frame_count = optional_int(value, "frameCount");
    attachment.slice_interval_ms = int_field(value, "sliceIntervalMs");
    attachment.page_count = optional_int(value, "pageCount");
    attachment.video_duration_ms = int_field(value, "videoDurationMs");
    attachment.file_name = string_field(value, "fileName");
    attachment.mime_type = string_field(value, "mimeType");
    attachment.file_size = int_field(value, "fileSize");
    attachment.pixel_width = optional_int(value, "pixelWidth");
    attachment.pixel_height = optional_int(value, "pixelHeight");
    attachment.processed_frames = string_list_field(value, "processedFrames");
    attachment.pre_rendered_paths = string_list_field(value, "preRenderedPaths");
    attachment.local_path = string_field(value, "localPath");
    attachment.storage = storage_of(string_field(value, "storage"));
    attachment.sandbox_path = string_field(value, "sandboxPath");
    attachment.import_state = import_state_of(string_field(value, "importState"));
    attachment.prepared_text = string_field(value, "preparedText");

    if (const json::Value* pages = field(value, "selectedPages"); !is_json_null(pages)) {
        std::vector<int> selected;
        for (const json::Value& element : pages->elements) {
            selected.push_back(static_cast<int>(std::stoll(element.text)));
        }
        attachment.selected_pages = selected;
    }
    if (const json::Value* unavailable = field(value, "unavailable"); !is_json_null(unavailable)) {
        attachment.unavailable = unavailable->text == "true";
    }
    return attachment;
}

std::vector<backup::SelectedAttachment> decode_attachments(const json::Value& root) {
    const json::Value* list = field(root, "attachments");
    if (is_json_null(list)) return {};
    std::vector<backup::SelectedAttachment> out;
    for (const json::Value& element : list->elements) out.push_back(decode_attachment(element));
    return out;
}

/** raw_json is re-serialized exactly like the driver's JsonElement.toString(). */
std::optional<std::string> raw_field(const json::Value& root) {
    if (const json::Value* raw_json = field(root, "raw_json")) {
        if (is_json_null(raw_json)) return std::nullopt;
        return json::to_kotlin_string(*raw_json);
    }
    return string_field(root, "raw");
}

using StringTable = std::map<std::string, std::optional<std::string>>;

StringTable string_table(const json::Value& root, std::string_view callback) {
    StringTable out;
    const json::Value* tables = field(root, "table");
    if (is_json_null(tables)) return out;
    const json::Value* table = field(*tables, callback);
    if (is_json_null(table)) return out;
    for (const json::Member& entry : table->members) {
        if (entry.value.kind == Kind::Literal && entry.value.text == "null") {
            out[entry.key] = std::nullopt;
        } else {
            out[entry.key] = entry.value.text;
        }
    }
    return out;
}

using PairTable = std::map<std::string, std::optional<std::pair<std::string, std::string>>>;

PairTable pair_table(const json::Value& root, std::string_view callback) {
    PairTable out;
    const json::Value* tables = field(root, "table");
    if (is_json_null(tables)) return out;
    const json::Value* table = field(*tables, callback);
    if (is_json_null(table)) return out;
    for (const json::Member& entry : table->members) {
        if (entry.value.kind == Kind::Literal && entry.value.text == "null") {
            out[entry.key] = std::nullopt;
        } else if (entry.value.elements.size() != 2) {
            scenario_fail(std::string(callback) + " table entry " + entry.key + " needs 2 elements");
        } else {
            out[entry.key] =
                std::make_pair(entry.value.elements[0].text, entry.value.elements[1].text);
        }
    }
    return out;
}

std::map<int, int> index_map(const json::Value& root, std::string_view name) {
    std::map<int, int> out;
    const json::Value* value = field(root, name);
    if (is_json_null(value)) return out;
    for (const json::Member& entry : value->members) {
        out[static_cast<int>(std::stoll(entry.key))] = static_cast<int>(std::stoll(entry.value.text));
    }
    return out;
}

std::map<int, std::string> int_string_map(const json::Value& root, std::string_view name) {
    std::map<int, std::string> out;
    const json::Value* value = field(root, name);
    if (is_json_null(value)) return out;
    for (const json::Member& entry : value->members) {
        out[static_cast<int>(std::stoll(entry.key))] = entry.value.text;
    }
    return out;
}

class Trace {
   public:
    void head(const std::string& name, const std::string& fn) {
        lines.push_back("name|" + esc(name));
        lines.push_back("fn|" + esc(fn));
    }
    void call(const std::string& callback, std::string_view arg,
              const std::optional<std::string>& result) {
        lines.push_back("call|" + esc(callback) + "|" + esc(arg) + "|" + opt(result));
    }
    void call_pair(const std::string& callback, std::string_view arg,
                   const std::optional<std::pair<std::string, std::string>>& result) {
        lines.push_back("call|" + esc(callback) + "|" + esc(arg) + "|" +
                        (result.has_value() ? esc(result->first + "|" + result->second)
                                            : std::string("-")));
    }
    void missing() { ++missing_count; }
    void attachments(const std::vector<backup::SelectedAttachment>& values) {
        std::string encoded = "[";
        for (const backup::SelectedAttachment& value : values) {
            if (encoded.size() > 1) encoded += ',';
            encoded += backup::encode_selected_attachment(value);
        }
        result("attachments", esc(encoded + "]"));
    }
    void strings(const std::vector<std::string>& values) {
        std::string encoded = "[";
        for (std::size_t at = 0; at < values.size(); ++at) {
            if (at != 0) encoded += ',';
            encoded += quote_json_string(values[at]);
        }
        result("strings", esc(encoded + "]"));
    }
    void string_result(const std::optional<std::string>& value) { result("string", opt(value)); }

    std::string render() const {
        std::string out;
        for (const std::string& line : lines) out += line + "\n";
        return out + "missing|" + std::to_string(missing_count) + "\n";
    }

   private:
    void result(const std::string& kind, const std::string& value) {
        lines.push_back("result|" + kind + "|" + value);
    }
    std::vector<std::string> lines;
    int missing_count = 0;
};

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string fn = required_field(root, "fn");
    Trace trace;
    trace.head(string_field(root, "name").value_or(stem), fn);

    const std::vector<backup::SelectedAttachment> attachments = decode_attachments(root);
    const StringTable archive_entry = string_table(root, "archiveEntryForSource");
    const StringTable restored_path = string_table(root, "restoredPathForArchiveEntry");
    const StringTable restored_uri = string_table(root, "restoredUriForArchiveEntry");
    const auto restored_primary = pair_table(root, "restoredPrimaryForArchiveEntry");
    const std::optional<std::string> raw = raw_field(root);

    const auto archive_source = [&](std::string_view source) -> std::optional<std::string> {
        const auto found = archive_entry.find(std::string(source));
        const std::optional<std::string> value =
            found == archive_entry.end() ? std::nullopt : found->second;
        trace.call("archiveEntryForSource", source, value);
        return value;
    };
    const auto restore_uri = [&](std::string_view entry) -> std::optional<std::string> {
        const auto found = restored_uri.find(std::string(entry));
        const std::optional<std::string> value =
            found == restored_uri.end() ? std::nullopt : found->second;
        trace.call("restoredUriForArchiveEntry", entry, value);
        return value;
    };
    const auto restore_path = [&](std::string_view entry) -> std::optional<std::string> {
        const auto found = restored_path.find(std::string(entry));
        const std::optional<std::string> value =
            found == restored_path.end() ? std::nullopt : found->second;
        trace.call("restoredPathForArchiveEntry", entry, value);
        return value;
    };

    if (fn == "exportableDraftAttachments") {
        trace.attachments(backup::NativeBackupMediaPolicy::exportable_draft_attachments(attachments));

    } else if (fn == "rewriteDraftAttachmentsForExport") {
        trace.attachments(backup::NativeBackupMediaPolicy::rewrite_draft_attachments_for_export(
            attachments, archive_source, [&trace]() { trace.missing(); }));

    } else if (fn == "restoreDraftAttachments") {
        trace.attachments(backup::NativeBackupMediaPolicy::restore_draft_attachments(
            attachments,
            [&](std::string_view entry) -> std::optional<std::pair<std::string, std::string>> {
                const auto found = restored_primary.find(std::string(entry));
                const std::optional<std::pair<std::string, std::string>> value =
                    found == restored_primary.end() ? std::nullopt : found->second;
                trace.call_pair("restoredPrimaryForArchiveEntry", entry, value);
                return value;
            },
            restore_path));

    } else if (fn == "toolImagePaths") {
        trace.strings(backup::NativeBackupMediaPolicy::tool_image_paths(raw));

    } else if (fn == "rewriteAttachmentMetaForExport") {
        const std::vector<std::string> sources =
            string_list_field(root, "originalImageSources").value_or(std::vector<std::string>{});
        trace.string_result(backup::NativeBackupMediaPolicy::rewrite_attachment_meta_for_export(
            raw, sources, index_map(root, "oldToNewImageIndex"), archive_source,
            [&trace]() { trace.missing(); }));

    } else if (fn == "rewriteToolImagePathsForExport") {
        trace.string_result(
            backup::NativeBackupMediaPolicy::rewrite_tool_image_paths_for_export(raw, archive_source));

    } else if (fn == "restoreAttachmentMeta") {
        trace.string_result(backup::NativeBackupMediaPolicy::restore_attachment_meta(
            raw, int_field_or(root, "archiveVersion", 4),
            index_map(root, "oldToNewImageIndex"), int_string_map(root, "legacyVideoUris"),
            restore_uri));

    } else if (fn == "restoreToolImagePaths") {
        trace.string_result(backup::NativeBackupMediaPolicy::restore_tool_image_paths(
            raw, int_field_or(root, "archiveVersion", 4), restore_path));

    } else {
        scenario_fail(stem + ": unknown function " + fn);
    }

    return trace.render();
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(200, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(backup_media_policy_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_BACKUP_MEDIA_POLICY_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}