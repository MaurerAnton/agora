// NativeBackupV5Baseline + NativeBackupV5Writer (data/NativeBackupV5Baseline.kt and
// data/NativeBackupV5Writer.kt).
//
// The baseline opens a previous archive through its central directory (the java.util.zip.ZipFile
// access model), validates the manifest and conversation index, and answers reuse questions after
// re-reading the referenced entries. copyRaw carries an entry's compressed stream into the output
// untouched, exactly like ZipArchiveOutputStream.addRawArchiveEntry. The writer assembles each
// conversation entry from streamed record strings with the Kotlin byte layout.
#include "agora/core/native_backup_v5.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "agora/core/json.hpp"
#include "agora/core/native_backup_format.hpp"

namespace agora::core::native_backup_v5 {
namespace {

namespace json = agora::core::json;
namespace native_backup = agora::core::native_backup;

constexpr std::int64_t kMaxMetadataBytes = 16LL * 1024LL * 1024LL;

const json::Value* member(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const auto& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

/** kotlinx integer literal: optional '-' followed by decimal digits, no fraction or exponent. */
std::int64_t integer_literal(const json::Value& value, std::string_view field) {
    if (value.kind != json::Kind::Literal) {
        throw std::runtime_error("field " + std::string(field) + " is not an integer");
    }
    const std::string& text = value.text;
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && text[index] == '-') {
        negative = true;
        ++index;
    }
    if (index == text.size()) {
        throw std::runtime_error("field " + std::string(field) + " is not an integer");
    }
    std::int64_t result = 0;
    for (; index < text.size(); ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') {
            throw std::runtime_error("field " + std::string(field) + " is not an integer");
        }
        const int value_digit = digit - '0';
        if (result > (INT64_MAX - value_digit) / 10) {
            throw std::runtime_error("field " + std::string(field) + " is out of range");
        }
        result = result * 10 + value_digit;
    }
    return negative ? -result : result;
}

std::int64_t required_long(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr) {
        throw std::runtime_error("missing field " + std::string(name));
    }
    return integer_literal(*value, name);
}

std::int64_t optional_long(const json::Value& object, std::string_view name,
                           std::int64_t fallback) {
    const json::Value* value = member(object, name);
    if (value == nullptr || value->is_literal("null")) return fallback;
    return integer_literal(*value, name);
}

std::string required_string(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || value->kind != json::Kind::String) {
        throw std::runtime_error("missing string field " + std::string(name));
    }
    return value->text;
}

std::vector<std::string> required_string_list(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || value->kind != json::Kind::Array) {
        throw std::runtime_error("missing array field " + std::string(name));
    }
    std::vector<std::string> result;
    result.reserve(value->elements.size());
    for (const auto& element : value->elements) {
        if (element.kind != json::Kind::String) {
            throw std::runtime_error("array field " + std::string(name) + " holds a non-string");
        }
        result.push_back(element.text);
    }
    return result;
}

std::vector<std::string> optional_string_list(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || value->is_literal("null")) return {};
    if (value->kind != json::Kind::Array) {
        throw std::runtime_error("field " + std::string(name) + " is not an array");
    }
    std::vector<std::string> result;
    result.reserve(value->elements.size());
    for (const auto& element : value->elements) {
        if (element.kind != json::Kind::String) {
            throw std::runtime_error("array field " + std::string(name) + " holds a non-string");
        }
        result.push_back(element.text);
    }
    return result;
}

json::Value parse_document(std::string_view text) {
    json::ParseResult parsed = json::parse(text);
    if (!parsed.value.has_value()) {
        throw std::runtime_error("malformed JSON at " + std::to_string(parsed.error_position));
    }
    return std::move(*parsed.value);
}

/** kotlinx Json with ignoreUnknownKeys = true, as used for the baseline metadata. */
ConversationIndex decode_index_document(const json::Value& document) {
    if (document.kind != json::Kind::Object) {
        throw std::runtime_error("conversation index is not an object");
    }
    const json::Value* conversations = member(document, "conversations");
    if (conversations == nullptr || conversations->kind != json::Kind::Array) {
        throw std::runtime_error("conversation index lacks conversations");
    }
    ConversationIndex index;
    index.conversations.reserve(conversations->elements.size());
    for (const auto& element : conversations->elements) {
        if (element.kind != json::Kind::Object) {
            throw std::runtime_error("conversation index entry is not an object");
        }
        ConversationIndexEntry entry;
        entry.id = required_string(element, "id");
        entry.data_changed_at = required_long(element, "dataChangedAt");
        entry.entry = required_string(element, "entry");
        entry.media_entries = optional_string_list(element, "mediaEntries");
        index.conversations.push_back(std::move(entry));
    }
    return index;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

void append_records(std::string& out, const std::vector<std::string>& records) {
    bool first = true;
    for (const auto& record : records) {
        if (!first) out.push_back(',');
        first = false;
        out += record;
    }
}

const zip::CentralEntry* find_entry(const std::vector<zip::CentralEntry>& central,
                                    const std::string& name) {
    const zip::CentralEntry* found = nullptr;
    for (const auto& entry : central) {
        if (entry.name == name) found = &entry;
    }
    return found;
}

/** ZipFile.getInputStream(...).bufferedReader().readText(), with the metadata size guards. */
std::string read_metadata(zip::Reader& reader, const std::vector<zip::CentralEntry>& central,
                          std::string_view name) {
    const zip::CentralEntry* entry = find_entry(central, std::string(name));
    if (entry == nullptr) {
        throw IOException("Missing baseline entry: " + std::string(name));
    }
    if (entry->size < 0 || entry->size > kMaxMetadataBytes) {
        throw std::runtime_error("baseline metadata entry is too large");
    }
    std::string text = reader.read_at_unchecked(entry->local_offset, entry->size);
    if (static_cast<std::int64_t>(text.size()) > kMaxMetadataBytes) {
        throw std::runtime_error("baseline metadata is too large");
    }
    return text;
}

int compare_text(const std::string& left, const std::string& right) {
    return left < right ? -1 : (right < left ? 1 : 0);
}

}  // namespace

// --------------------------------------------------------------------- Baseline

Baseline::Baseline(zip::Reader reader, ConversationIndex index,
                   std::vector<zip::CentralEntry> central)
    : reader_(std::move(reader)), index_(std::move(index)), central_(std::move(central)) {
    for (std::size_t position = 0; position < central_.size(); ++position) {
        by_name_[central_[position].name] = position;
    }
    for (std::size_t position = 0; position < index_.conversations.size(); ++position) {
        by_id_[index_.conversations[position].id] = position;
    }
}

std::optional<Baseline> Baseline::open_or_null(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
    try {
        zip::Reader reader(path);
        std::vector<zip::CentralEntry> central = reader.central_entries();

        const std::string manifest_text =
            read_metadata(reader, central, native_backup::kManifestEntry);
        const json::Value& manifest = parse_document(manifest_text);
        if (manifest.kind != json::Kind::Object) return std::nullopt;

        const std::int64_t version = required_long(manifest, "agora_export_version");
        const std::vector<std::string> categories = required_string_list(manifest, "categories");
        const std::int64_t incremental = optional_long(manifest, "incremental_baseline", 0);

        if (version != native_backup::kCurrentVersion) return std::nullopt;
        if (incremental != native_backup::kIncrementalBaselineRevision) return std::nullopt;
        if (std::find(categories.begin(), categories.end(), "conversations") ==
            categories.end()) {
            return std::nullopt;
        }

        const std::string index_text =
            read_metadata(reader, central, native_backup::kConversationIndexEntry);
        ConversationIndex index = decode_index_document(parse_document(index_text));

        std::set<std::string> ids;
        for (const ConversationIndexEntry& item : index.conversations) {
            if (!ids.insert(item.id).second) return std::nullopt;
            if (item.entry != native_backup::conversation_entry(item.id)) return std::nullopt;
            if (find_entry(central, item.entry) == nullptr) return std::nullopt;
            for (const std::string& media : item.media_entries) {
                if (find_entry(central, media) == nullptr) return std::nullopt;
            }
        }
        return Baseline(std::move(reader), std::move(index), std::move(central));
    } catch (...) {
        return std::nullopt;
    }
}

bool Baseline::can_reuse(const std::string& id, std::int64_t data_changed_at) {
    auto found = by_id_.find(id);
    if (found == by_id_.end()) return false;
    const ConversationIndexEntry& item = index_.conversations[found->second];
    if (item.data_changed_at != data_changed_at) return false;
    if (!is_intact(item.entry)) return false;
    for (const std::string& media : item.media_entries) {
        if (!is_intact(media)) return false;
    }
    return true;
}

bool Baseline::is_intact(const std::string& name) {
    auto cached = intact_.find(name);
    if (cached != intact_.end()) return cached->second;
    bool result = false;
    const zip::CentralEntry* entry = find(name);
    if (entry != nullptr) {
        try {
            const std::string content =
                reader_.read_at_unchecked(entry->local_offset, entry->size);
            result = static_cast<std::int64_t>(content.size()) == entry->size &&
                     zip::crc32(content) == entry->crc32;
        } catch (...) {
            result = false;
        }
    }
    intact_[name] = result;
    return result;
}

void Baseline::copy_raw(const std::string& name, zip::Writer& output) {
    const zip::CentralEntry* entry = find(name);
    if (entry == nullptr) throw IOException("Missing baseline entry: " + name);
    const std::string raw = reader_.raw_at(entry->local_offset, entry->compressed_size);
    output.put_raw_entry(entry->name, entry->method, entry->crc32, entry->compressed_size,
                         entry->size, raw);
}

const ConversationIndexEntry* Baseline::index_entry(const std::string& id) const {
    auto found = by_id_.find(id);
    if (found == by_id_.end()) return nullptr;
    return &index_.conversations[found->second];
}

const zip::CentralEntry* Baseline::find(const std::string& name) const {
    auto found = by_name_.find(name);
    if (found == by_name_.end()) return nullptr;
    return &central_[found->second];
}

// --------------------------------------------------------------------- writer

ConversationIndexEntry write_conversation_entry(zip::Writer& zip,
                                                const std::string& conversation_id,
                                                std::int64_t data_changed_at,
                                                std::string_view conversation_json,
                                                const std::vector<std::string>& runs,
                                                const std::vector<std::string>& messages,
                                                const std::vector<std::string>& loops,
                                                std::vector<std::string> media_entries) {
    ConversationIndexEntry result;
    result.id = conversation_id;
    result.data_changed_at = data_changed_at;
    result.entry = native_backup::conversation_entry(conversation_id);
    result.media_entries = std::move(media_entries);
    std::sort(result.media_entries.begin(), result.media_entries.end(), compare_text);

    std::string content = "{\"conversations\":[";
    content += conversation_json;
    content += "],\"runs\":[";
    append_records(content, runs);
    content += "],\"messages\":[";
    append_records(content, messages);
    content += "],\"loops\":[";
    append_records(content, loops);
    content += "]}";
    zip.put_entry(result.entry, content);
    return result;
}

void write_tasks_entry(zip::Writer& zip, const std::vector<std::string>& tasks) {
    std::string content = "{\"tasks\":[";
    append_records(content, tasks);
    content += "]}";
    zip.put_entry(native_backup::kTasksEntry, content);
}

void write_conversation_index(zip::Writer& zip,
                              const std::vector<ConversationIndexEntry>& index) {
    ConversationIndex document;
    document.conversations = index;
    zip.put_entry(native_backup::kConversationIndexEntry, encode_index(document));
}

ConversationIndexEntry copy_conversation_from_baseline(zip::Writer& zip, Baseline& baseline,
                                                       const ConversationIndexEntry& baseline_entry,
                                                       std::vector<std::string>& copied_media) {
    baseline.copy_raw(baseline_entry.entry, zip);
    for (const std::string& media : baseline_entry.media_entries) {
        if (std::find(copied_media.begin(), copied_media.end(), media) == copied_media.end()) {
            copied_media.push_back(media);
            baseline.copy_raw(media, zip);
        }
    }
    return baseline_entry;
}

std::string encode_index(const ConversationIndex& index) {
    std::string out = "{\"conversations\":[";
    bool first = true;
    for (const ConversationIndexEntry& item : index.conversations) {
        if (!first) out.push_back(',');
        first = false;
        out += "{\"id\":";
        append_quoted(out, item.id);
        out += ",\"dataChangedAt\":";
        out += std::to_string(item.data_changed_at);
        out += ",\"entry\":";
        append_quoted(out, item.entry);
        if (!item.media_entries.empty()) {
            out += ",\"mediaEntries\":[";
            bool first_media = true;
            for (const std::string& media : item.media_entries) {
                if (!first_media) out.push_back(',');
                first_media = false;
                append_quoted(out, media);
            }
            out.push_back(']');
        }
        out.push_back('}');
    }
    out += "]}";
    return out;
}

ConversationIndex decode_index(std::string_view text) {
    return decode_index_document(parse_document(text));
}

}  // namespace agora::core::native_backup_v5
