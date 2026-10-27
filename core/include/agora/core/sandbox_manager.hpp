#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/alpine_package_metadata_store.hpp"
#include "agora/core/sandbox_executor.hpp"
#include "agora/core/sandbox_path_resolver.hpp"
#include "agora/core/shell_file_edit.hpp"
#include "agora/core/shell_tool_backends.hpp"
#include "agora/core/shell_tools.hpp"

namespace agora::core::sandbox {

/** SandboxManager.PackageInfo (P:/V:/T: of the apk installed DB). */
struct SandboxPackageInfo {
    std::string name;
    std::string version;
    std::int64_t size_bytes = 0;
    std::string description;
};

/**
 * ProotSandboxManager: the C++ port of the SandboxManager transport
 * (app/src/fdroid/java/com/newoether/agora/sandbox/ProotSandboxManager.kt + SandboxManager.kt).
 *
 * It implements the transport interface SandboxBackend drives; file operations delegate to the
 * already-ported sandbox::file_operations over one shared SandboxPathResolver, so path
 * resolution, containment and read/write/glob/grep/edit semantics stay pinned by the
 * sandbox-path-resolver stand.
 *
 * Responsibilities:
 *  - availability: rootfs + /bin/sh repair + musl linker + mount targets + root home rewrite;
 *  - command execution: proot argv assembly + bounded output/timeout (executor overridable so
 *    tests and non-Android hosts can script it);
 *  - package metadata: AlpinePackageMetadataStore, apk_list parse, apk_delete guard;
 *  - read-only file ops are unserialized; write/edit take the mutation mutex (Kotlin contract).
 *
 * Platform boundaries (documented, not ported): rootfs download/extract and the APKINDEX
 * network fetch for apk_install/apk_upgrade — they require the host network stack.
 */
class ProotSandboxManager final : public shell_backends::SandboxManager {
public:
    /**
     * Command executor: (command, workdir, timeout_ms) → result. Empty → default proot
     * executor (run_bounded_process with the Config's proot paths).
     */
    using Executor = std::function<SandboxResult(const std::string& command,
                                                 const std::string& workdir,
                                                 std::int32_t timeout_ms)>;

    struct Config {
        std::filesystem::path rootfs_dir;
        std::filesystem::path home_mount_dir;
        std::string home_mount_path = "/home/agora";
        std::string shared_storage_mount_path = "/mnt/shared";
        /** Host shared-storage dir provider (nullopt → /mnt/shared not bound). */
        std::function<std::optional<std::filesystem::path>()> shared_storage_host_dir;

        // Default-executor inputs (unused when `executor` is set).
        std::filesystem::path proot_path;
        std::filesystem::path native_lib_dir;
        std::filesystem::path talloc_lib_dir;
        std::string proot_loader_name = "libproot_loader.so";

        Executor executor;
    };

    explicit ProotSandboxManager(Config config);
    ~ProotSandboxManager() override;

    ProotSandboxManager(const ProotSandboxManager&) = delete;
    ProotSandboxManager& operator=(const ProotSandboxManager&) = delete;

    // ── shell_backends::SandboxManager (SandboxBackend surface) ────────────────
    bool is_available() override;
    SandboxResult execute_command(const std::string& command, const std::string& workdir,
                                  std::int32_t timeout_ms) override;
    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit) override;
    std::optional<std::string> file_write(const std::string& path,
                                          const std::string& content) override;
    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& old_string,
                                              const std::string& new_string,
                                              bool replace_all) override;
    std::pair<std::vector<std::string>, bool> file_glob(
        const std::string& pattern, const std::string& base_path,
        const std::optional<std::int32_t>& depth) override;
    shell_backends::GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                                          const std::string& file_glob) override;

    // ── Kotlin SandboxManager surface (synchronous core) ───────────────────────
    /** isAvailable(): mutation-serialized availability check with repair writes. */
    bool prepare_availability();

    /** isAvailableSync(): fast check, no mutex, no repair. */
    bool is_available_sync() const;

    /** Parsed lib/apk/db/installed (empty when the sandbox is unavailable). */
    std::vector<SandboxPackageInfo> apk_list();

    /**
     * apkDelete: metadata-guarded removal — sanitize, refuse base packages, keep the
     * explicit-packages/world consistent, run `apk del` through the executor and verify.
     */
    bool apk_delete(const std::string& package_name);

    /** Rootfs disk usage in MiB (Kotlin getDiskUsageMB). */
    std::int64_t disk_usage_mb();

    /** reset(): delete the rootfs (retry x3, 200 ms apart); true when it is gone. */
    bool reset();

    const std::string& last_error() const noexcept { return last_error_; }

    // ── Test seams / composition ───────────────────────────────────────────────
    AlpinePackageMetadataStore& package_metadata() noexcept { return *package_metadata_; }
    SandboxPathResolver& resolver() noexcept { return resolver_; }
    /** Exposed for the mutation contract: file_write/file_edit serialize on this mutex. */
    std::mutex& mutation_mutex() noexcept { return mutation_mutex_; }

private:
    Config config_;
    std::mutex mutation_mutex_;       // serializes state-mutating ops (Kotlin mutationMutex)
    SandboxPathResolver resolver_;
    std::unique_ptr<AlpinePackageMetadataStore> package_metadata_;
    std::string last_error_;

    /** prepareAvailability body — caller holds mutation_mutex_. */
    bool prepare_availability_unlocked();
    bool ensure_shell();
    void ensure_root_home();
    void ensure_sandbox_mount_targets();

    /** executeRaw: proot argv + env assembly over the executor (default: run_bounded_process). */
    SandboxResult execute_raw(const std::string& command, const std::string& workdir,
                              std::int32_t timeout_ms);

    /** Default proot run: assembly + result mapping (timeout → "Timed out", truncation warning). */
    SandboxResult run_proot(const std::string& command, const std::string& workdir,
                            std::int32_t timeout_ms) const;
};

}  // namespace agora::core::sandbox