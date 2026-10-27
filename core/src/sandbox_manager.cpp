#include "agora/core/sandbox_manager.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <thread>

namespace agora::core::sandbox {

/** The base-transport result type (nested in shell_backends::SandboxManager). */
using SandboxResult = shell_backends::SandboxManager::SandboxResult;

namespace {

constexpr std::size_t kShellCommandOutputMaxBytes = 1u << 20;  // SHELL_COMMAND_OUTPUT_MAX_BYTES
constexpr std::int32_t kApkDeleteTimeoutMs = 60000;

/** Kotlin shellQuote: single quotes with the '\'' escape. */
std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string trim_copy(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool blank(const std::string& value) { return trim_copy(value).empty(); }

}  // namespace

ProotSandboxManager::ProotSandboxManager(Config config)
    : config_(std::move(config)),
      resolver_(config_.rootfs_dir, config_.home_mount_dir, config_.home_mount_path,
                config_.shared_storage_mount_path, config_.shared_storage_host_dir),
      package_metadata_(std::make_unique<AlpinePackageMetadataStore>(config_.rootfs_dir)) {}

ProotSandboxManager::~ProotSandboxManager() = default;

// ── Availability ─────────────────────────────────────────────────────────────────

bool ProotSandboxManager::ensure_shell() {
    namespace fs = std::filesystem;
    const fs::path sh = config_.rootfs_dir / "bin/sh";
    std::error_code ec;
    if (fs::exists(sh, ec)) return true;
    try {
        const fs::path busybox = config_.rootfs_dir / "bin/busybox";
        if (fs::is_regular_file(busybox, ec)) {
            // Delete a broken symlink first (exists()==false but the entry is present).
            fs::remove(sh, ec);
            fs::copy_file(busybox, sh, fs::copy_options::none);
            fs::permissions(sh, fs::perms::owner_exec, fs::perm_options::add);
            return true;
        }
    } catch (...) {
        std::error_code remove_ec;
        fs::remove(sh, remove_ec);
    }
    return false;
}

void ProotSandboxManager::ensure_root_home() {
    namespace fs = std::filesystem;
    const fs::path passwd = config_.rootfs_dir / "etc/passwd";
    std::error_code ec;
    if (!fs::is_regular_file(passwd, ec)) return;
    std::ifstream input(passwd, std::ios::binary);
    if (!input) return;
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (content.find("root:x:0:0:root:/home/agora:") != std::string::npos) return;

    // Kotlin: Regex("^(root:x:0:0:root:)/root(:)", MULTILINE) → "/home/agora" for "/root".
    static const std::string kPrefix = "root:x:0:0:root:";
    static const std::string kRootSuffix = "/root:";
    std::string updated;
    std::size_t line_start = 0;
    while (line_start <= content.size()) {
        const std::size_t line_end = content.find('\n', line_start);
        const bool has_newline = line_end != std::string::npos;
        const std::size_t end = has_newline ? line_end : content.size();
        const std::string line = content.substr(line_start, end - line_start);
        if (line.rfind(kPrefix, 0) == 0 &&
            line.size() >= kPrefix.size() + kRootSuffix.size() &&
            line.compare(kPrefix.size(), kRootSuffix.size(), kRootSuffix) == 0) {
            updated.append(line, 0, kPrefix.size());
            updated += "/home/agora";
            updated.append(line, kPrefix.size() + kRootSuffix.size() - 1,
                           std::string::npos);  // keep the ':'
        } else {
            updated += line;
        }
        if (!has_newline) break;
        updated += '\n';
        line_start = end + 1;
    }
    if (updated != content) {
        std::ofstream out(passwd, std::ios::binary | std::ios::trunc);
        out << updated;
    }
}

void ProotSandboxManager::ensure_sandbox_mount_targets() {
    resolver_.ensure_sandbox_mount_targets();
}

bool ProotSandboxManager::prepare_availability_unlocked() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(config_.rootfs_dir, ec)) {
        last_error_ = "rootfs not found: " + config_.rootfs_dir.string();
        return false;
    }
    if (!ensure_shell()) {
        last_error_ = "/bin/sh missing";
        return false;
    }
    const bool linker = fs::exists(config_.rootfs_dir / "lib/ld-musl-aarch64.so.1", ec) ||
                        fs::exists(config_.rootfs_dir / "usr/lib/ld-musl-aarch64.so.1", ec);
    if (!linker) {
        last_error_ = "musl linker missing";
        return false;
    }
    ensure_sandbox_mount_targets();
    package_metadata_->ensure_package_metadata();
    ensure_root_home();
    return true;
}

bool ProotSandboxManager::prepare_availability() {
    std::lock_guard<std::mutex> lock(mutation_mutex_);
    return prepare_availability_unlocked();
}

bool ProotSandboxManager::is_available() { return prepare_availability(); }

bool ProotSandboxManager::is_available_sync() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(config_.rootfs_dir, ec)) return false;
    if (!fs::exists(config_.rootfs_dir / "bin/sh", ec)) return false;
    return fs::exists(config_.rootfs_dir / "lib/ld-musl-aarch64.so.1", ec) ||
           fs::exists(config_.rootfs_dir / "usr/lib/ld-musl-aarch64.so.1", ec);
}

// ── Command execution ────────────────────────────────────────────────────────────

SandboxResult ProotSandboxManager::execute_command(const std::string& command,
                                                                   const std::string& workdir,
                                                                   std::int32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutation_mutex_);
    if (!prepare_availability_unlocked()) {
        return SandboxResult{"", "Sandbox not installed", -1, std::nullopt};
    }
    return execute_raw(command, workdir.empty() ? config_.home_mount_path : workdir, timeout_ms);
}

SandboxResult ProotSandboxManager::execute_raw(const std::string& command,
                                                               const std::string& workdir,
                                                               std::int32_t timeout_ms) {
    // Kotlin executeRaw repairs the shell and mount targets before every run.
    ensure_shell();
    ensure_sandbox_mount_targets();
    if (config_.executor) return config_.executor(command, workdir, timeout_ms);
    return run_proot(command, workdir, timeout_ms);
}

SandboxResult ProotSandboxManager::run_proot(const std::string& command,
                                                             const std::string& workdir,
                                                             std::int32_t timeout_ms) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(config_.rootfs_dir / "tmp", ec);

    // proot argv assembly — mirrors ProotSandboxManager.executeRaw.
    std::vector<std::string> argv;
    argv.push_back(config_.proot_path.string());
    argv.push_back("--rootfs=" + config_.rootfs_dir.string());
    argv.push_back("--bind=/dev");
    argv.push_back("--bind=/proc");
    argv.push_back("--bind=/sys");
    argv.push_back("--bind=/dev/urandom:/dev/random");
    argv.push_back("--bind=" + config_.home_mount_dir.string() + ":" + config_.home_mount_path);
    if (const auto shared = config_.shared_storage_host_dir ? config_.shared_storage_host_dir()
                                                            : std::nullopt) {
        argv.push_back("--bind=" + shared->string() + ":" + config_.shared_storage_mount_path);
    }
    argv.push_back("-w");
    argv.push_back(workdir);
    argv.push_back("-0");
    argv.push_back("--link2symlink");
    argv.push_back("--kill-on-exit");
    argv.push_back("-L");
    argv.push_back("/bin/sh");
    argv.push_back("-c");
    argv.push_back(command);

    std::vector<std::string> env;
    env.push_back("LD_LIBRARY_PATH=" + config_.talloc_lib_dir.string() + ":" +
                  config_.native_lib_dir.string());
    env.push_back("PROOT_LOADER=" + (config_.native_lib_dir / config_.proot_loader_name).string());
    env.push_back("PROOT_TMP_DIR=" + (config_.rootfs_dir / "tmp").string());
    env.push_back("HOME=" + config_.home_mount_path);
    env.push_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");

    ProcessResult run;
    try {
        run = run_bounded_process(argv, env, timeout_ms, kShellCommandOutputMaxBytes);
    } catch (const std::exception& error) {
        return SandboxResult{"", error.what(), -1, std::nullopt};
    }

    SandboxResult result;
    result.stdout_text = std::move(run.output);
    result.exit_code = run.exit_code;
    result.stderr_text = run.timed_out ? "Timed out" : "";
    if (run.truncated) {
        result.warning = "Local Sandbox output was truncated at " +
                         std::to_string(kShellCommandOutputMaxBytes) + " UTF-8 bytes.";
    }
    return result;
}

// ── File operations (delegate to the pinned sandbox::file_operations) ────────────

shell_tools::ShellFileReadResult ProotSandboxManager::file_read(const std::string& path,
                                                                std::int64_t offset,
                                                                std::int64_t limit) {
    return file_operations::file_read(resolver_, path, offset, limit);
}

std::optional<std::string> ProotSandboxManager::file_write(const std::string& path,
                                                           const std::string& content) {
    return file_operations::file_write(resolver_, path, content, mutation_mutex_);
}

shell_file::ShellFileEditResult ProotSandboxManager::file_edit(const std::string& path,
                                                               const std::string& old_string,
                                                               const std::string& new_string,
                                                               bool replace_all) {
    return file_operations::file_edit(resolver_, path, old_string, new_string, replace_all,
                                      mutation_mutex_);
}

std::pair<std::vector<std::string>, bool> ProotSandboxManager::file_glob(
    const std::string& pattern, const std::string& base_path,
    const std::optional<std::int32_t>& depth) {
    return file_operations::file_glob(resolver_, pattern, base_path, config_.home_mount_path,
                                      depth);
}

shell_backends::GrepOutcome ProotSandboxManager::file_grep(const std::string& pattern,
                                                           const std::string& base_path,
                                                           const std::string& file_glob) {
    // Kotlin Regex(pattern) and resolveSandboxPath both fail into Result.failure(e) —
    // pre-run them here so the outcome carries the exception message like the Kotlin one.
    try {
        const std::regex probe(pattern);
    } catch (const std::regex_error& error) {
        return shell_backends::GrepOutcome{false, {}, false, error.what()};
    }
    try {
        resolver_.resolve_sandbox_path(base_path.empty() ? config_.home_mount_path : base_path);
    } catch (const std::exception& error) {
        return shell_backends::GrepOutcome{false, {}, false, error.what()};
    }
    const auto result =
        file_operations::file_grep(resolver_, pattern, base_path, config_.home_mount_path,
                                   file_glob);
    if (!result) {
        return shell_backends::GrepOutcome{false, {}, false, "fileGrep failed"};
    }
    shell_backends::GrepOutcome outcome;
    outcome.ok = true;
    outcome.truncated = result->second;
    for (const auto& match : result->first) {
        outcome.matches.push_back(shell_backends::GrepMatch{match.path, match.line,
                                                            match.content});
    }
    return outcome;
}

// ── Package management ───────────────────────────────────────────────────────────

std::vector<SandboxPackageInfo> ProotSandboxManager::apk_list() {
    std::vector<SandboxPackageInfo> packages;
    if (!prepare_availability()) return packages;  // Kotlin: isAvailable() gate (locks inside)

    namespace fs = std::filesystem;
    const fs::path db = config_.rootfs_dir / "lib/apk/db/installed";
    std::error_code ec;
    if (!fs::is_regular_file(db, ec)) return packages;

    std::ifstream input(db, std::ios::binary);
    if (!input) return packages;
    std::string name;
    std::string version;
    std::string description;
    const auto flush = [&] {
        if (!name.empty()) {
            packages.push_back(SandboxPackageInfo{name, version, 0, description});
        }
        name.clear();
        version.clear();
        description.clear();
    };
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("P:", 0) == 0) {
            name = trim_copy(line.substr(2));
        } else if (line.rfind("V:", 0) == 0) {
            version = trim_copy(line.substr(2));
        } else if (line.rfind("T:", 0) == 0) {
            description = trim_copy(line.substr(2));
        } else if (trim_copy(line).empty()) {
            flush();
        }
    }
    flush();
    return packages;
}

bool ProotSandboxManager::apk_delete(const std::string& package_name) {
    std::lock_guard<std::mutex> lock(mutation_mutex_);
    if (!prepare_availability_unlocked()) return false;

    std::string requested;
    try {
        requested = AlpinePackageMetadataStore::sanitize_package_name(package_name);
    } catch (const std::invalid_argument& error) {
        last_error_ = error.what();
        return false;
    }
    last_error_.clear();
    package_metadata_->ensure_package_metadata();

    const auto installed_before = package_metadata_->read_installed_versions();
    if (!AlpinePackageMetadataStore::contains(installed_before, requested)) {
        // Not installed: drop any stale explicit entry and re-normalize world.
        auto explicit_packages = package_metadata_->read_explicit_packages();
        explicit_packages.erase(
            std::remove(explicit_packages.begin(), explicit_packages.end(), requested),
            explicit_packages.end());
        package_metadata_->write_explicit_packages(explicit_packages);
        package_metadata_->normalize_world(explicit_packages);
        return true;
    }

    if (package_metadata_->is_base_package(requested)) {
        last_error_ = "Refusing to remove base package: " + requested;
        return false;
    }

    const auto previous_explicit = package_metadata_->read_explicit_packages();
    std::vector<std::string> next_explicit = previous_explicit;
    next_explicit.erase(std::remove(next_explicit.begin(), next_explicit.end(), requested),
                        next_explicit.end());
    package_metadata_->write_explicit_packages(next_explicit);
    package_metadata_->normalize_world(next_explicit);

    const SandboxResult result = execute_raw("apk del " + shell_quote(requested),
                                             config_.home_mount_path, kApkDeleteTimeoutMs);
    const bool removed =
        !AlpinePackageMetadataStore::contains(package_metadata_->read_installed_versions(),
                                              requested);
    if (!removed) {
        package_metadata_->write_explicit_packages(previous_explicit);
        package_metadata_->normalize_world(previous_explicit);
        if (!blank(result.stderr_text)) {
            last_error_ = trim_copy(result.stderr_text);
        } else if (!blank(result.stdout_text)) {
            last_error_ = trim_copy(result.stdout_text);
        } else {
            last_error_ = "Package was not removed: " + requested;
        }
        return false;
    }
    package_metadata_->normalize_world();
    return result.exit_code == 0 || removed;
}

std::int64_t ProotSandboxManager::disk_usage_mb() {
    try {
        std::int64_t total = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(config_.rootfs_dir, ec), end;
             it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code size_ec;
            total += static_cast<std::int64_t>(it->file_size(size_ec));
        }
        if (ec) throw std::runtime_error(ec.message());
        return total / (1024 * 1024);
    } catch (...) {
        return 0;
    }
}

bool ProotSandboxManager::reset() {
    std::lock_guard<std::mutex> lock(mutation_mutex_);
    namespace fs = std::filesystem;
    try {
        for (int attempt = 1; attempt <= 3; ++attempt) {
            std::error_code ec;
            fs::remove_all(config_.rootfs_dir, ec);
            if (!fs::exists(config_.rootfs_dir, ec)) break;
            if (attempt < 3) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (fs::exists(config_.rootfs_dir)) {
            last_error_ = "Cannot delete sandbox rootfs";
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        last_error_ = error.what();
        return false;
    }
}

}  // namespace agora::core::sandbox