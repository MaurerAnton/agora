#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::memory {

/**
 * Native port of data/MemoryManager.kt (plus its DescriptionMetadataStore).
 *
 * The golden stand (core/tools/golden/memory-manager) compiles the app's Kotlin sources verbatim
 * and freezes one trace per scenario; the parity test replays every scenario here and requires the
 * emitted trace to match byte for byte. Error messages are part of the contract: this class throws
 * std::runtime_error carrying the exact message the Kotlin `require` would produce.
 *
 * Android's Context is reduced to the single value it contributes: the base files directory.
 */
class MemoryManager {
public:
    struct MemoryFileInfo {
        std::string name;         // path relative to memory_db, '/'-separated
        std::string description;  // from memory_meta.json, empty when unset
    };

    /** @param files_dir stand-in for Context.filesDir. */
    explicit MemoryManager(std::string files_dir);
    ~MemoryManager();

    // ---- active memory (active_memory.md next to memory_db) -------------------------------------
    std::string get_active_memory() const;

    /** @return the Kotlin result message ("Active memory updated.", ...). */
    std::string update_active_memory(std::string_view content,
                                     std::string_view mode = "replace",
                                     const std::optional<std::string>& old_string = std::nullopt,
                                     const std::optional<std::string>& new_string = std::nullopt);

    // ---- per-file descriptions (memory_db/memory_meta.json) -------------------------------------
    std::string get_description(std::string_view name) const;
    void set_description(std::string_view name, std::string_view description);

    std::vector<MemoryFileInfo> list_files() const;

    std::string get_meta_json() const;
    void save_meta_json(std::string_view json_string);

    // ---- catalog files ---------------------------------------------------------------------------
    std::string read_file(std::string_view name) const;

    /** @return "Created <relative path>". */
    std::string create_file(std::string_view name, std::string_view content,
                            std::string_view description = "");

    /**
     * Mirrors Kotlin editFile's named parameters. Exactly the same validation order and messages,
     * including the "Updated <target name>" / "No changes made." result.
     */
    std::string edit_file(std::string_view name,
                          const std::optional<std::string>& content = std::nullopt,
                          const std::optional<std::string>& new_name = std::nullopt,
                          const std::optional<std::string>& description = std::nullopt,
                          const std::optional<std::string>& old_string = std::nullopt,
                          const std::optional<std::string>& new_string = std::nullopt);

    /** @return "Deleted <file name>". */
    std::string delete_file(std::string_view name);

    std::int64_t active_memory_revision() const;
    std::int64_t catalog_revision() const;

private:
    class MetadataStore;

    mutable std::mutex mutex_;
    std::filesystem::path memory_dir_;       // <files_dir>/memory_db
    std::filesystem::path active_memory_;    // <files_dir>/active_memory.md
    std::filesystem::path meta_file_;        // memory_db/memory_meta.json
    std::unique_ptr<MetadataStore> metadata_;

    std::int64_t active_memory_revision_ = 0;
    std::int64_t catalog_revision_ = 0;

    std::string get_active_memory_unlocked() const;
    std::string relative_path(const std::filesystem::path& file) const;
    std::filesystem::path resolve_file(std::string_view name) const;
    void prune_empty_parents(const std::filesystem::path& dir) const;

    /** Shared by create/edit: writes text and syncs it to disk like Kotlin's writeText + fsync. */
    static void write_text(const std::filesystem::path& path, std::string_view content);
};

}  // namespace agora::core::memory
