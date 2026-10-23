#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::skill {

/**
 * Native port of data/SkillManager.kt (plus the shared data/DescriptionMetadataStore.kt).
 *
 * The golden stand (core/tools/golden/skill-manager) compiles the app's Kotlin sources verbatim
 * and freezes one trace per scenario; the parity test replays every scenario here and requires
 * the emitted trace to match byte for byte. Error messages are part of the contract: this class
 * throws std::runtime_error / std::invalid_argument carrying the exact message the Kotlin
 * `require` would produce.
 *
 * Android's Context is reduced to the single value it contributes: the base files directory.
 * The skill catalog is a flat directory (`<files_dir>/skill_db`) — unlike MemoryManager it never
 * walks subdirectories, and descriptions are keyed by the bare file name.
 */
class SkillManager {
public:
    struct SkillFileInfo {
        std::string name;
        std::string description;  // from skill_meta.json, empty when unset
    };

    /** @param files_dir stand-in for Context.filesDir. */
    explicit SkillManager(std::string files_dir);
    ~SkillManager();

    /** Kotlin listFiles(): skill_db entries whose extension is exactly "md", sorted by name. */
    std::vector<SkillFileInfo> list_files() const;

    /** Kotlin catalog(): the <available_skills> preload block, or "" when empty. */
    std::string catalog() const;

    std::string get_description(std::string_view name) const;
    std::string get_meta_json() const;
    void save_meta_json(std::string_view json_string);

    std::string read_file(std::string_view name) const;

    /** @return "Created <file name>". */
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

    std::int64_t catalog_revision() const;

private:
    class MetadataStore;

    mutable std::mutex mutex_;
    std::filesystem::path skill_dir_;  // <files_dir>/skill_db
    std::filesystem::path meta_file_;  // skill_db/skill_meta.json
    std::unique_ptr<MetadataStore> metadata_;

    std::int64_t catalog_revision_ = 0;

    std::filesystem::path resolve_file(std::string_view name) const;

    /** catalog() already holds the instance lock (JVM `synchronized` is reentrant). */
    std::vector<SkillFileInfo> list_files_unlocked() const;
};

}  // namespace agora::core::skill
