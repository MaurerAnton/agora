#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/conversation_export_snapshot_reader.hpp"
#include "agora/core/export_extra_settings.hpp"
#include "agora/core/export_spool.hpp"
#include "agora/core/memory_manager.hpp"
#include "agora/core/native_backup_secrets.hpp"
#include "agora/core/native_backup_v5.hpp"
#include "agora/core/portable_settings_archive.hpp"
#include "agora/core/skill_manager.hpp"
#include "agora/core/zip_writer.hpp"

namespace agora::core::data_export {

/** Export categories, matching data/ExportCategory.kt manifest keys in declaration order. */
enum class ExportCategory {
    Conversations,
    Memories,
    SystemPrompts,
    Settings,
    ApiKeys,
};

inline std::string_view manifest_key(ExportCategory category) {
    switch (category) {
        case ExportCategory::Conversations: return "conversations";
        case ExportCategory::Memories:      return "memories";
        case ExportCategory::SystemPrompts: return "system_prompts";
        case ExportCategory::Settings:      return "settings";
        case ExportCategory::ApiKeys:       return "api_keys";
    }
    return "";
}

inline std::optional<ExportCategory> category_from_manifest_key(std::string_view key) {
    if (key == "conversations") return ExportCategory::Conversations;
    if (key == "memories")      return ExportCategory::Memories;
    if (key == "system_prompts") return ExportCategory::SystemPrompts;
    if (key == "settings")      return ExportCategory::Settings;
    if (key == "api_keys")      return ExportCategory::ApiKeys;
    return std::nullopt;
}

/** Result of a single export invocation. */
struct ExportResult {
    int images_exported = 0;
    int missing_resource_count = 0;
};

/** Platform surface the export depends on. All callbacks must be thread-safe if used concurrently. */
struct Platform {
    /** Application version string (from PackageManager). */
    std::function<std::string()> app_version;

    /** ISO-8601 instant (from Instant.now()). */
    std::function<std::string()> exported_at;

    /** Creates a new temporary spool file; returns its path. The file is created empty.
     *  The exporter writes to it and deletes it on completion (even on failure). */
    std::function<std::string()> create_spool_file;

    /** Generates a new random UUID (lowercase, standard format). */
    std::function<std::string()> new_uuid;

    /** Opens an output sink for the final archive. Returns nullopt on failure. */
    class OutputSink {
    public:
        virtual ~OutputSink() = default;
        virtual void write(std::string_view bytes) = 0;
    };
    std::function<std::unique_ptr<OutputSink>()> open_output;

    /** Cancellation/ensureActive hook; may throw. */
    std::function<void()> ensure_active;

    // --- Media access ---
    /** Returns true if a bare file path exists. Used for media_source_key and draft sources. */
    std::function<bool(std::string_view)> file_exists;

    /** Canonical path of a bare file; may throw (e.g., if path is invalid). */
    std::function<std::string(std::string_view)> canonical_path;

    /** Absolute path fallback when canonical_path fails. */
    std::function<std::string(std::string_view)> absolute_path;

    /**
     * Opens a media source (content://, file://, or bare path). Returns the bytes of the stream,
     * or nullopt if the source cannot be opened (counted as missing resource). Throws on read
     * failures after the stream was opened (mirrors Kotlin: open failures are null, copy failures throw).
     */
    std::function<std::optional<std::string>(std::string_view source)> open_media;
};

/** A system prompt entry (data/SystemPromptEntry.kt). Field order matches Kotlin declaration. */
struct PromptTemplateItem {
    std::string id;
    std::string type;   // "CUSTOM" or "PREDEFINED"
    std::string value;
};

struct SystemPromptEntry {
    std::string id;                      // always encoded (default is UUID.randomUUID())
    std::string title;
    std::string content = "";            // omitted when empty (default)
    std::vector<PromptTemplateItem> system_items;
    std::vector<PromptTemplateItem> user_items;
    std::vector<PromptTemplateItem> assistant_items;
    std::vector<PromptTemplateItem> user_prepend_items;
    std::vector<PromptTemplateItem> user_postpend_items;
};

/** Input dependencies that are data, not callbacks. */
struct Inputs {
    const settings_archive::SettingsSnapshot& settings;
    const std::map<std::string, export_extra_settings::ConversationSettings>& conversation_settings;
    const std::vector<SystemPromptEntry>& system_prompts;
    memory::MemoryManager& memory;
    skill::SkillManager& skill;
    /**
     * The SettingsManager read surface NativeBackupSecretsPolicy.capture drives; the caller
     * supplies the real store (or the golden double) exactly like the Kotlin call site does.
     * Read only when the api_keys category is actually exported with include_api_keys.
     */
    const native_backup::SettingsStore* secrets_store = nullptr;
};

/** Request parameters for a single export. */
struct ExportRequest {
    std::vector<ExportCategory> categories;   // order preserved
    bool include_api_keys = false;
    std::optional<std::string> baseline_file; // path to an existing archive
};

/** Progress callback: receives the float exactly as Kotlin's onProgress (0..1). */
using ProgressCallback = std::function<void(float)>;

/** Kotlin exception categories with exact simple names for golden parity. */
class IOException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    const char* kotlin_type() const noexcept { return "IOException"; }
};

/**
 * Main entry point. Runs one export to the platform's output sink.
 *
 * The caller owns the lifetime of all inputs and callbacks. The exporter calls
 * `ensure_active()` at the same points as Kotlin (before each conversation).
 *
 * Throws IOException("Could not open the selected backup destination") if open_output returns
 * nullopt. Propagates any exception thrown by callbacks (including ensure_active).
 */
ExportResult export_data(conversation_export_snapshot::SnapshotStore& store,
                         const Inputs& inputs,
                         const Platform& platform,
                         const ExportRequest& request,
                         const ProgressCallback& on_progress = {});

}  // namespace agora::core::data_export