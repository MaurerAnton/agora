#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/chat_dao.hpp"
#include "agora/core/data_exporter.hpp"
#include "agora/core/export_extra_settings.hpp"
#include "agora/core/memory_manager.hpp"
#include "agora/core/native_backup_archive.hpp"
#include "agora/core/native_backup_format.hpp"
#include "agora/core/native_backup_secrets.hpp"
#include "agora/core/native_conversation_graph_importer.hpp"
#include "agora/core/native_conversation_graph_source.hpp"
#include "agora/core/portable_settings_archive.hpp"
#include "agora/core/skill_manager.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::data_import {

/** Exception thrown when cancellation is requested (mirrors Kotlin's CancellationException). */
class Cancellation : public std::runtime_error {
public:
    explicit Cancellation(const std::string& msg = "Cancelled") : std::runtime_error(msg) {}
};

/** Import strategy for each category, matching DataImporter.ImportStrategy. */
enum class ImportStrategy { Merge, Replace, Skip };

/** Manifest read from the backup archive. */
struct ImportManifest {
    int version = 1;
    std::string app_version;
    std::string exported_at;
    std::vector<std::string> categories;
    bool has_api_keys = false;
};

/** Preview result showing what an archive contains without decompressing conversations. */
struct ImportPreview {
    ImportManifest manifest;
    int conversation_count = 0;
    int task_count = 0;
    int memory_count = 0;
    int system_prompt_count = 0;
    bool settings_present = false;
    bool api_keys_present = false;

    bool has_conversation_graph() const noexcept {
        return conversation_count > 0 || task_count > 0;
    }
    bool has_importable_data() const noexcept {
        return has_conversation_graph() || memory_count > 0 || system_prompt_count > 0 ||
               settings_present || api_keys_present;
    }
    bool is_supported_version() const noexcept {
        return native_backup::is_supported(manifest.version);
    }
};

/** Result of a full import operation. */
struct ImportResult {
    int conversations_imported = 0;
    int tasks_imported = 0;
    int loops_imported = 0;
    int memories_imported = 0;
    int system_prompts_imported = 0;
    bool settings_imported = false;
    bool api_keys_imported = false;
    std::vector<std::string> errors;
};

/** Platform surface for file/media operations needed by the import orchestration. */
struct Platform {
    /** Files directory (context.filesDir) where media and custom fonts are written. */
    std::filesystem::path files_dir;

    /** Package name for FileProvider URIs. */
    std::string package_name;

    /** Callback to generate a new UUID string (used for media file names, temp font files, etc.). */
    std::function<std::string()> new_uuid;

    /** Callback to generate a FileProvider content URI for a file. */
    std::function<std::string(const std::filesystem::path&)> file_provider_uri;

    /** Callback returning available bytes on the destination volume (for copy_to checks). */
    std::function<std::int64_t()> available_bytes = []() { return INT64_MAX; };
};

/** Settings view: the subset of SettingsManager flows DataImporter reads during import. */
struct SettingsView {
    std::function<std::vector<data_export::SystemPromptEntry>()> system_prompts;
    std::function<std::vector<native_backup::EmbeddingModelConfig>()> embedding_models;
    std::function<std::string()> active_embedding_model_id;
    std::function<std::string()> font_preference;
    std::function<std::string()> custom_font_path;
    std::function<std::string()> custom_font_name;

    /** Builds a full SettingsSnapshot for PortableSettingsArchive.restore_from_json_object. */
    std::function<settings_archive::SettingsSnapshot()> archive_snapshot;
};

/** Settings writes: the sink interfaces DataImporter calls during import. */
struct SettingsWrites {
    settings_archive::SettingsSink* archive_sink = nullptr;          // PortableSettingsArchive
    export_extra_settings::SettingsStore* legacy_store = nullptr;    // ExportExtraSettings
    native_backup::SettingsStore* secrets_store = nullptr;           // NativeBackupSecretsPolicy

    /** saveSystemPrompts is not in any existing sink; called directly by DataImporter. */
    std::function<void(const std::vector<data_export::SystemPromptEntry>&)> save_system_prompts;
};

/** Conversation settings transfer coordination (applies pending imports from the graph importer). */
class ConversationSettingsTransfer {
public:
    ConversationSettingsTransfer(chat_dao::ChatDao& dao,
                                 std::function<void()> await_initial_load,
                                 std::function<void(const std::vector<std::pair<std::string, export_extra_settings::ConversationSettings>>&, bool)> apply_import);

    /** Called at the start of staging to apply any pending import transfer from a previous run. */
    void complete_pending_import();

    /** Called after a successful graph import to apply the new transfer and delete it. */
    void complete_import(const std::string& transfer_id);

private:
    chat_dao::ChatDao& dao_;
    std::function<void()> await_initial_load_;
    std::function<void(const std::vector<std::pair<std::string, export_extra_settings::ConversationSettings>>&, bool)> apply_import_;
};

/** Main DataImporter class: ports data/DataImporter.kt. */
class DataImporter {
public:
    DataImporter(
        chat_dao::ChatDao& chat,
        semantic_index::SemanticIndexDao& semantic,
        maintenance_debt::MaintenanceDebtDao& debt,
        SqlExecutor& sql,
        memory::MemoryManager& memory,
        skill::SkillManager& skill,
        SettingsView settings_view,
        SettingsWrites settings_writes,
        ConversationSettingsTransfer& conversation_settings,
        Platform platform);

    /** Preview an archive without importing (reads manifest, index, counts). */
    ImportPreview preview(native_backup_archive::NativeBackupArchive& archive);

    /** Full import with progress callbacks. */
    ImportResult import(native_backup_archive::NativeBackupArchive& archive,
                        const std::map<data_export::ExportCategory, ImportStrategy>& decisions,
                        const std::function<void(float)>& on_progress = {});

private:
    chat_dao::ChatDao& chat_;
    semantic_index::SemanticIndexDao& semantic_;
    maintenance_debt::MaintenanceDebtDao& debt_;
    SqlExecutor& sql_;
    memory::MemoryManager& memory_;
    skill::SkillManager& skill_;
    SettingsView settings_;
    SettingsWrites writes_;
    ConversationSettingsTransfer& transfers_;
    Platform platform_;

    native_graph::NativeConversationGraphSource graph_source_;
    native_graph_import::NativeConversationGraphImporter graph_importer_;

    // Internal helpers (private methods)
    std::optional<settings_archive::CustomFont> restore_custom_font(
        native_backup_archive::NativeBackupArchive& archive,
        int archive_version);

    std::vector<data_export::SystemPromptEntry> plan_system_prompts(
        native_backup_archive::NativeBackupArchive& archive,
        ImportStrategy strategy);

    struct PromptPlan {
        std::vector<data_export::SystemPromptEntry> prompts;
        std::map<std::string, std::string> id_map;
        std::vector<std::string> available_ids;
    };

    struct StagedGraph {
        native_graph::NativeConversationGraphSource::Stream stream;
        native_graph_import::RestoredMedia media;
        native_graph_import::ConversationGraphHeaders headers;
    };

    std::optional<StagedGraph> stage_conversation_graph(
        native_backup_archive::NativeBackupArchive& archive,
        int version,
        ImportStrategy strategy,
        const PromptPlan& prompt_plan);

    void reconcile_imported_embedding_models(
        const std::vector<native_backup::EmbeddingModelConfig>& previous_models);
};

}  // namespace agora::core::data_import
