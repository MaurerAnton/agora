#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace agora::core::sandbox {

/**
 * Port of app/src/fdroid/java/com/newoether/agora/sandbox/AlpinePackageMetadataStore.kt.
 *
 * Pure file-level metadata over the Alpine rootfs:
 *   etc/agora/base-world, etc/agora/explicit-packages, etc/apk/world, lib/apk/db/installed.
 *
 * Order semantics: Kotlin uses LinkedHashMap/LinkedHashSet, so every file-backed collection
 * keeps insertion (file) order — vectors here, not sorted maps.
 */
class AlpinePackageMetadataStore {
public:
    /** name → version for lib/apk/db/installed, in DB record order. */
    using InstalledVersions = std::vector<std::pair<std::string, std::string>>;

    explicit AlpinePackageMetadataStore(std::filesystem::path rootfs_dir);

    /**
     * sanitizePackageName: trims and enforces `^[A-Za-z0-9][A-Za-z0-9+_.:-]*$`.
     * Throws std::invalid_argument("Invalid package name: …") like Kotlin's require().
     */
    static std::string sanitize_package_name(const std::string& package_name);

    /** True when [name] is present in [versions]. */
    static bool contains(const InstalledVersions& versions, const std::string& name);

    /** P:/V: records of lib/apk/db/installed, in file order (empty when missing). */
    InstalledVersions read_installed_versions();

    /** etc/apk/world lines, trimmed and non-empty, in file order. */
    std::vector<std::string> read_world_lines();
    void write_world_lines(const std::vector<std::string>& lines);

    /** Capture the base-world snapshot (idempotent unless force). */
    void capture_base_world(bool force = false);

    /** capture_base_world(false) then the stored base-world lines. */
    std::vector<std::string> read_base_world();

    /** Whether [package_name] appears in the base world (name compared without version pin). */
    bool is_base_package(const std::string& package_name);

    /** etc/agora/explicit-packages lines that still sanitize (file order). */
    std::vector<std::string> read_explicit_packages();
    void write_explicit_packages(const std::vector<std::string>& packages);

    /** Migrate world → explicit-packages when missing, then normalize_world(). */
    void ensure_package_metadata();

    /** base world followed by explicit packages not already pinned as base. */
    void normalize_world();
    void normalize_world(const std::vector<std::string>& explicit_packages);

    /** Add a package to explicit-packages and re-normalize world. */
    void add_explicit_package(const std::string& package_name);

    const std::filesystem::path& rootfs_dir() const noexcept { return rootfs_dir_; }

    /** The five packages the Kotlin default base world falls back to. */
    static const std::vector<std::string>& default_base_world();

private:
    std::filesystem::path rootfs_dir_;

    std::filesystem::path metadata_dir() const { return rootfs_dir_ / "etc/agora"; }
    std::filesystem::path base_world_file() const { return metadata_dir() / "base-world"; }
    std::filesystem::path explicit_packages_file() const { return metadata_dir() / "explicit-packages"; }
    std::filesystem::path world_file() const { return rootfs_dir_ / "etc/apk/world"; }
    std::filesystem::path installed_db_file() const { return rootfs_dir_ / "lib/apk/db/installed"; }
};

/** Kotlin `worldPackageName`: trims, drops a leading '!', cuts at the first pin operator. */
std::string world_package_name(const std::string& line);

}  // namespace agora::core::sandbox