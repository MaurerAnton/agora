#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/zip_reader.hpp"
#include "agora/core/zip_writer.hpp"

namespace agora::core::native_backup_v5 {

/** java.io.IOException — the baseline's missing-entry failure. */
class IOException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** NativeConversationIndexEntry: one emitted conversation item. */
struct ConversationIndexEntry {
    std::string id;
    std::int64_t data_changed_at = 0;
    std::string entry;
    std::vector<std::string> media_entries;
};

/** NativeConversationIndex: the `conv/index.json` payload. */
struct ConversationIndex {
    std::vector<ConversationIndexEntry> conversations;
};

/**
 * NativeBackupV5Baseline: read-only view of a previous version 5 archive.
 *
 * `open_or_null` validates the manifest (current version, incremental revision, conversations
 * category), the unique conversation ids, the derived entry names and the presence of every
 * referenced item and media entry — any failure yields nullopt, exactly like the Kotlin
 * `runCatching { ... }.getOrNull()`. Entries are addressed through the central directory, like
 * java.util.zip.ZipFile.
 */
class Baseline {
public:
    static std::optional<Baseline> open_or_null(const std::string& path);

    const ConversationIndex& index() const { return index_; }

    /**
     * True when `id`'s baseline item was written at `data_changed_at` and it and every media
     * entry it references still decompress to their recorded size and CRC.
     */
    bool can_reuse(const std::string& id, std::int64_t data_changed_at);

    /** Carries the named entry into `output` with its compressed stream untouched. */
    void copy_raw(const std::string& name, zip::Writer& output);

    const ConversationIndexEntry* index_entry(const std::string& id) const;

private:
    Baseline(zip::Reader reader, ConversationIndex index, std::vector<zip::CentralEntry> central);

    const zip::CentralEntry* find(const std::string& name) const;
    bool is_intact(const std::string& name);

    zip::Reader reader_;
    ConversationIndex index_;
    std::vector<zip::CentralEntry> central_;
    std::map<std::string, std::size_t> by_name_;
    std::map<std::string, std::size_t> by_id_;
    std::map<std::string, bool> intact_;
};

/**
 * NativeBackupV5Writer.writeConversationEntry: writes one `conv/items/<hex-id>.json` entry from
 * already archive-rewritten JSON records and returns its index entry (media entries sorted).
 */
ConversationIndexEntry write_conversation_entry(zip::Writer& zip, const std::string& conversation_id,
                                                std::int64_t data_changed_at,
                                                std::string_view conversation_json,
                                                const std::vector<std::string>& runs,
                                                const std::vector<std::string>& messages,
                                                const std::vector<std::string>& loops,
                                                std::vector<std::string> media_entries);

/** NativeBackupV5Writer.writeTasksEntry: writes `conv/tasks.json` from raw task records. */
void write_tasks_entry(zip::Writer& zip, const std::vector<std::string>& tasks);

/** NativeBackupV5Writer.writeConversationIndex: writes `conv/index.json`. */
void write_conversation_index(zip::Writer& zip, const std::vector<ConversationIndexEntry>& index);

/**
 * NativeBackupV5Writer.copyConversationFromBaseline: raw-copies an unchanged item and its media,
 * adding each media entry to `copied_media` exactly once.
 */
ConversationIndexEntry copy_conversation_from_baseline(zip::Writer& zip, Baseline& baseline,
                                                       const ConversationIndexEntry& baseline_entry,
                                                       std::vector<std::string>& copied_media);

/**
 * kotlinx encoding of NativeConversationIndex with the default Json configuration
 * (encodeDefaults = false, declaration field order).
 */
std::string encode_index(const ConversationIndex& index);

/** kotlinx decoding of NativeConversationIndex (ignoreUnknownKeys = true). */
ConversationIndex decode_index(std::string_view text);

}  // namespace agora::core::native_backup_v5
