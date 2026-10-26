#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/sandbox_virtual_paths.hpp"
#include "agora/core/shell_tools.hpp"
#include "agora/core/shell_file_edit.hpp"

namespace agora::core::sandbox {

/**
 * Physical file resolved from a virtual sandbox path, with the root and virtual prefix
 * that produced it. Mirrors `ResolvedSandboxPath` from SandboxPathResolver.kt.
 */
struct ResolvedSandboxPath {
    std::filesystem::path file;
    std::filesystem::path physical_root;
    std::string virtual_root;  // "/", "/mnt/home", or "/mnt/shared"
};

/**
 * SandboxPathResolver: resolves virtual sandbox paths (e.g. "/mnt/home/foo.txt") to
 * physical filesystem paths under the correct mount point, with traversal containment.
 *
 * The Kotlin version uses `java.io.File.canonicalFile` for symlink resolution and the
 * containment check. This C++ port uses `std::filesystem::canonical` and checks that
 * the resolved path is within the allowed physical root.
 *
 * Mount targets (homeMountDir, sharedStorageMountDir) are created lazily on first use.
 */
class SandboxPathResolver {
public:
    /**
     * @param rootfs_dir             Root filesystem directory (e.g. /data/data/.../rootfs)
     * @param home_mount_dir         Host directory mounted as /mnt/home inside the sandbox
     * @param home_mount_path        Virtual path for home mount (e.g. "/mnt/home")
     * @param shared_storage_mount_path  Virtual path for shared storage (e.g. "/mnt/shared")
     * @param shared_storage_host_dir  Callable returning the host shared storage dir, or null
     */
    SandboxPathResolver(std::filesystem::path rootfs_dir,
                        std::filesystem::path home_mount_dir,
                        std::string home_mount_path,
                        std::string shared_storage_mount_path,
                        std::function<std::optional<std::filesystem::path>()> shared_storage_host_dir);

    /**
     * Ensure the mount target directories exist in the rootfs.
     */
    void ensure_sandbox_mount_targets();

    /**
     * Resolve a virtual sandbox path to a physical path with traversal checks.
     * Throws std::runtime_error("Path traversal: ...") on containment violation.
     */
    ResolvedSandboxPath resolve_sandbox_path(std::string_view path);

    /**
     * Resolve a virtual sandbox path to a physical path (throws on traversal).
     */
    std::filesystem::path resolve_path(std::string_view path);

    /**
     * Walk files under `dir` (a physical directory) and append virtual paths to `result`.
     * `physical_root_abs_path` must be the canonical absolute path of the physical root.
     * `virtual_root` is the virtual prefix ("/", "/mnt/home", "/mnt/shared").
     * `remaining`: -1 = unlimited recursion; 0 = unlimited; 1 = this dir only; >1 = descend with one fewer.
     */
    void walk_virtual_files(const std::filesystem::path& dir,
                            std::vector<std::string>& result,
                            std::string_view physical_root_abs_path,
                            std::string_view virtual_root,
                            int remaining = -1);

    // Accessors for the mount paths
    const std::string& home_mount_path() const noexcept { return home_mount_path_; }
    const std::string& shared_storage_mount_path() const noexcept { return shared_storage_mount_path_; }

private:
    std::filesystem::path rootfs_dir_;
    std::filesystem::path home_mount_dir_;
    std::string home_mount_path_;
    std::string shared_storage_mount_path_;
    std::function<std::optional<std::filesystem::path>()> shared_storage_host_dir_;
};

/**
 * File operations on the sandbox filesystem, using SandboxPathResolver for path resolution.
 * Mirrors SandboxFileOperations.kt.
 *
 * These functions are designed to be called from the tool execution layer. The `mutation_mutex`
 * parameter serializes write/edit operations on the same resolver instance.
 */
namespace file_operations {

/**
 * Read a bounded file slice. Returns ShellFileReadResult with content, line counts,
 * and metadata. Throws std::runtime_error on failure.
 */
shell_tools::ShellFileReadResult file_read(SandboxPathResolver& resolver,
                                           std::string_view path,
                                           std::int64_t offset = 0,
                                           std::int64_t limit = 0);

/**
 * Write a file atomically. Returns nullopt on success, error message on failure.
 * Serialized by `mutation_mutex`.
 */
std::optional<std::string> file_write(SandboxPathResolver& resolver,
                                      std::string_view path,
                                      std::string_view content,
                                      std::mutex& mutation_mutex);

/**
 * List files matching a glob pattern. Returns (matches, truncated).
 * `depth`: null/-1 = unlimited recursion; 0 = unlimited; 1 = this dir only; >1 = max levels.
 */
std::pair<std::vector<std::string>, bool> file_glob(SandboxPathResolver& resolver,
                                                    std::string_view pattern,
                                                    std::string_view base_path,
                                                    std::string_view home_mount_path,
                                                    std::optional<int> depth);

/**
 * Grep for a regex pattern in files. Returns Result<(matches, truncated)>.
 * `matches` are SandboxManager::GrepMatch compatible (path, line, content).
 */
struct GrepMatch {
    std::string path;
    std::int32_t line = 0;
    std::string content;
};

using GrepResult = std::pair<std::vector<GrepMatch>, bool>;
std::optional<GrepResult> file_grep(SandboxPathResolver& resolver,
                                    std::string_view pattern,
                                    std::string_view base_path,
                                    std::string_view home_mount_path,
                                    std::string_view file_glob);

/**
 * Edit a file atomically. Returns ShellFileEditResult (replacements, sha256, or error).
 * Serialized by `mutation_mutex`.
 */
agora::core::shell_file::ShellFileEditResult file_edit(SandboxPathResolver& resolver,
                                          std::string_view path,
                                          std::string_view old_string,
                                          std::string_view new_string,
                                          bool replace_all,
                                          std::mutex& mutation_mutex);

}  // namespace file_operations

}  // namespace agora::core::sandbox