#include "agora/core/sandbox_path_resolver.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "agora/core/utf16.hpp"

namespace agora::core::sandbox {

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

/**
 * Fully resolve a path, including `..` and `.` components, even if the final
 * component doesn't exist. This mimics Java's `File.canonicalFile()` behavior.
 *
 * The existing prefix runs as far as components exist so `canonical()` resolves
 * every symlink inside it (Java resolves symlinks in the existing part); only the
 * missing tail is appended manually with `..`/`.` handling.
 */
static std::filesystem::path fully_resolve_path(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path result;
    
    // Split into existing prefix and non-existing suffix
    std::filesystem::path existing_prefix;
    std::filesystem::path remaining;
    bool missing_tail = false;
    
    // Find the longest existing prefix
    for (auto it = p.begin(); it != p.end(); ++it) {
        if (!missing_tail) {
            std::filesystem::path candidate = existing_prefix / *it;
            if (std::filesystem::exists(candidate, ec)) {
                existing_prefix = candidate;
                continue;
            }
            missing_tail = true;
        }
        remaining /= *it;
    }
    
    // If we didn't find any existing prefix, use the root
    if (existing_prefix.empty()) {
        existing_prefix = p.root_path();
        remaining = p.relative_path();
    }
    
    // Canonicalize the existing prefix
    auto canon_prefix = std::filesystem::canonical(existing_prefix, ec);
    if (ec) {
        throw std::runtime_error("canonical failed for " + existing_prefix.string() + ": " + ec.message());
    }
    
    // Now manually resolve the remaining components
    result = canon_prefix;
    for (const auto& component : remaining) {
        if (component == "..") {
            if (result.has_parent_path()) {
                result = result.parent_path();
            }
        } else if (component == "." || component.empty()) {
            // Skip
        } else {
            result /= component;
        }
    }
    
    return result;
}

static std::filesystem::path canonical_or_throw(const std::filesystem::path& p) {
    if (p.empty()) {
        throw std::runtime_error("canonical_or_throw called with empty path");
    }
    return fully_resolve_path(p);
}

static bool is_within_root(const std::filesystem::path& resolved,
                           const std::filesystem::path& root) {
    // Both paths must be canonical absolute paths.
    // Check if resolved == root or resolved starts with root + separator.
    if (resolved == root) return true;
    auto it_resolved = resolved.begin();
    auto it_root = root.begin();
    for (; it_root != root.end() && it_resolved != resolved.end(); ++it_root, ++it_resolved) {
        if (*it_root != *it_resolved) return false;
    }
    return it_root == root.end();  // all root components matched
}

// -----------------------------------------------------------------------------
// SandboxPathResolver
// -----------------------------------------------------------------------------

SandboxPathResolver::SandboxPathResolver(
    std::filesystem::path rootfs_dir,
    std::filesystem::path home_mount_dir,
    std::string home_mount_path,
    std::string shared_storage_mount_path,
    std::function<std::optional<std::filesystem::path>()> shared_storage_host_dir)
    : rootfs_dir_(std::move(rootfs_dir))
    , home_mount_dir_(std::move(home_mount_dir))
    , home_mount_path_(std::move(home_mount_path))
    , shared_storage_mount_path_(std::move(shared_storage_mount_path))
    , shared_storage_host_dir_(std::move(shared_storage_host_dir)) {}

void SandboxPathResolver::ensure_sandbox_mount_targets() {
    std::error_code ec;
    std::filesystem::create_directories(home_mount_dir_, ec);
    // Create the mount points inside rootfs
    auto home_in_rootfs = rootfs_dir_ / home_mount_path_.substr(1);  // strip leading '/'
    std::filesystem::create_directories(home_in_rootfs, ec);
    auto shared_in_rootfs = rootfs_dir_ / shared_storage_mount_path_.substr(1);
    std::filesystem::create_directories(shared_in_rootfs, ec);
}

ResolvedSandboxPath SandboxPathResolver::resolve_sandbox_path(std::string_view path) {
    std::string normalized = normalize_virtual_path(path);

    // Home mount: /mnt/home or /mnt/home/...
    if (normalized == home_mount_path_ ||
        (normalized.size() > home_mount_path_.size() &&
         normalized.compare(0, home_mount_path_.size(), home_mount_path_) == 0 &&
         normalized[home_mount_path_.size()] == '/')) {
        ensure_sandbox_mount_targets();
        auto root = canonical_or_throw(home_mount_dir_);
        std::string suffix = normalized.substr(home_mount_path_.size());
        if (!suffix.empty() && suffix[0] == '/') suffix = suffix.substr(1);
        auto resolved = canonical_or_throw(root / suffix);
        if (!is_within_root(resolved, root)) {
            throw std::runtime_error("Path traversal: " + std::string(path));
        }
        return ResolvedSandboxPath{resolved, root, home_mount_path_};
    }

    // Shared storage mount: /mnt/shared or /mnt/shared/...
    if (normalized == shared_storage_mount_path_ ||
        (normalized.size() > shared_storage_mount_path_.size() &&
         normalized.compare(0, shared_storage_mount_path_.size(), shared_storage_mount_path_) == 0 &&
         normalized[shared_storage_mount_path_.size()] == '/')) {
        auto host_dir_opt = shared_storage_host_dir_();
        if (!host_dir_opt) {
            throw std::runtime_error(
                "Shared storage is not mounted. Enable it and grant all-files access first.");
        }
        auto root = canonical_or_throw(*host_dir_opt);
        std::string suffix = normalized.substr(shared_storage_mount_path_.size());
        if (!suffix.empty() && suffix[0] == '/') suffix = suffix.substr(1);
        auto resolved = canonical_or_throw(root / suffix);
        if (!is_within_root(resolved, root)) {
            throw std::runtime_error("Path traversal: " + std::string(path));
        }
        return ResolvedSandboxPath{resolved, root, shared_storage_mount_path_};
    }

    // Default: rootfs
    auto root = canonical_or_throw(rootfs_dir_);
    std::string suffix = normalized;
    if (!suffix.empty() && suffix[0] == '/') suffix = suffix.substr(1);
    auto resolved = canonical_or_throw(root / suffix);
    if (!is_within_root(resolved, root)) {
        throw std::runtime_error("Path traversal: " + std::string(path));
    }
    return ResolvedSandboxPath{resolved, root, "/"};
}

std::filesystem::path SandboxPathResolver::resolve_path(std::string_view path) {
    return resolve_sandbox_path(path).file;
}

void SandboxPathResolver::walk_virtual_files(const std::filesystem::path& dir,
                                             std::vector<std::string>& result,
                                             std::string_view physical_root_abs_path,
                                             std::string_view virtual_root,
                                             int remaining) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;

        auto filename = entry.path().filename().string();
        if (filename == "." || filename == "..") continue;

        auto full_path = entry.path();
        if (entry.is_directory(ec)) {
            if (remaining < 0 || remaining > 1) {
                walk_virtual_files(full_path, result, physical_root_abs_path, virtual_root,
                                   remaining < 0 ? -1 : remaining - 1);
            }
        } else if (entry.is_regular_file(ec)) {
            try {
                auto canonical_path = canonical_or_throw(full_path);
                std::string path_str = canonical_path.string();
                std::string root_str = std::string(physical_root_abs_path);

                // Remove prefix
                if (path_str.size() > root_str.size() &&
                    path_str.compare(0, root_str.size(), root_str) == 0) {
                    path_str = path_str.substr(root_str.size());
                }
                // Remove leading separator
                if (!path_str.empty() && path_str[0] == std::filesystem::path::preferred_separator) {
                    path_str = path_str.substr(1);
                }
                // Convert to forward slashes
                std::replace(path_str.begin(), path_str.end(),
                             std::filesystem::path::preferred_separator, '/');

                std::string prefix;
                if (virtual_root != "/") {
                    prefix = std::string(virtual_root);
                    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
                }
                result.push_back(prefix + "/" + path_str);
            } catch (const std::exception&) {
                // Skip files that can't be canonicalized
            }
        }
    }
}

// -----------------------------------------------------------------------------
// file_operations
// -----------------------------------------------------------------------------

namespace file_operations {

shell_tools::ShellFileReadResult file_read(SandboxPathResolver& resolver,
                                           std::string_view path,
                                           std::int64_t offset,
                                           std::int64_t limit) {
    auto file = resolver.resolve_path(path);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        throw std::runtime_error("File not found: " + std::string(path));
    }

    std::uintmax_t total_bytes = std::filesystem::file_size(file, ec);
    if (ec) {
        throw std::runtime_error("Failed to get file size: " + ec.message());
    }

    constexpr std::int64_t SHELL_FILE_READ_MAX_BYTES = 1'048'576L;
    std::int64_t normalized_offset = std::max<std::int64_t>(offset, 0);
    std::int64_t start = std::min(normalized_offset, static_cast<std::int64_t>(total_bytes));
    std::int64_t effective_limit = (limit >= 1 && limit <= SHELL_FILE_READ_MAX_BYTES)
                                       ? limit
                                       : SHELL_FILE_READ_MAX_BYTES;
    std::int64_t requested_bytes = std::min(effective_limit, static_cast<std::int64_t>(total_bytes) - start);

    std::vector<char> buffer(requested_bytes > 0 ? requested_bytes : 0);
    std::size_t bytes_read = 0;
    if (requested_bytes > 0) {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            throw std::runtime_error("Failed to open file for reading: " + file.string());
        }
        input.seekg(start);
        if (!input) {
            throw std::runtime_error("Failed to seek to offset " + std::to_string(start));
        }
        input.read(buffer.data(), requested_bytes);
        bytes_read = input.gcount();
        if (input.bad()) {
            throw std::runtime_error("Error reading file");
        }
    }

    std::string content(buffer.data(), bytes_read);

    // Compute totalLines if file is small enough
    std::int32_t total_lines = 0;
    if (total_bytes <= SHELL_FILE_READ_MAX_BYTES) {
        std::ifstream full_input(file, std::ios::binary);
        std::string full_content((std::istreambuf_iterator<char>(full_input)),
                                 std::istreambuf_iterator<char>());
        total_lines = shell_tools::shell_file_line_count(full_content);
    }

    shell_tools::ShellFileReadResult result;
    result.content = std::move(content);
    result.lines = shell_tools::shell_file_line_count(result.content);
    result.total_lines = total_lines;
    result.total_bytes = static_cast<std::int64_t>(total_bytes);
    result.returned_bytes = static_cast<std::int64_t>(result.content.size());
    result.offset = start;
    result.limit = effective_limit;
    result.truncated = (start + static_cast<std::int64_t>(bytes_read) < static_cast<std::int64_t>(total_bytes));
    return result;
}

static std::optional<std::string> write_sandbox_file_atomically(
    const std::filesystem::path& file,
    const std::vector<char>& content,
    std::function<std::optional<std::string>()> before_replace) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !std::filesystem::is_regular_file(file, ec)) {
        return "path is not a regular file";
    }
    auto parent = file.parent_path();
    if (parent.empty()) {
        return "file has no parent directory";
    }
    if (!std::filesystem::exists(parent, ec) && !std::filesystem::create_directories(parent, ec)) {
        return "failed to create parent directory";
    }

    // Preserve mode if file exists
    std::uintmax_t mode = 0;
    if (std::filesystem::exists(file, ec)) {
#if defined(_WIN32)
        mode = 0x1A4;  // default 0644 equivalent
#else
        // On POSIX, we can't easily get mode via filesystem. Use default.
        mode = 0x1A4;
#endif
    } else {
        mode = 0x1A4;
    }

    // Create temp file in same directory
    std::string temp_name = ".tmp." + file.filename().string() + ".";
    std::filesystem::path temp_path;
    {
        std::error_code temp_ec;
        // Use a unique temp file name
        for (int i = 0; i < 100; ++i) {
            temp_path = parent / (temp_name + std::to_string(i));
            if (!std::filesystem::exists(temp_path, temp_ec)) break;
        }
    }

    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            return "failed to create temporary file";
        }
        output.write(content.data(), content.size());
        if (!output) {
            return "failed to write temporary file";
        }
        output.flush();
        // Sync to disk
        output.close();
    }

    // Set mode on temp file
#if !defined(_WIN32)
    chmod(temp_path.c_str(), static_cast<mode_t>(mode));
#endif

    // beforeReplace callback
    if (auto err = before_replace()) {
        std::error_code rm_ec;
        std::filesystem::remove(temp_path, rm_ec);
        return *err;
    }

    // Atomic rename
    std::error_code rename_ec;
    std::filesystem::rename(temp_path, file, rename_ec);
    if (rename_ec) {
        std::error_code rm_ec;
        std::filesystem::remove(temp_path, rm_ec);
        return "failed to rename temporary file: " + rename_ec.message();
    }

    return std::nullopt;
}

std::optional<std::string> file_write(SandboxPathResolver& resolver,
                                      std::string_view path,
                                      std::string_view content,
                                      std::mutex& mutation_mutex) {
    std::lock_guard<std::mutex> lock(mutation_mutex);

    constexpr std::size_t SHELL_FILE_WRITE_MAX_BYTES = 1'048'576;
    if (content.size() > SHELL_FILE_WRITE_MAX_BYTES) {
        return "content exceeds 1MB limit";
    }

    try {
        auto file = resolver.resolve_path(path);
        std::vector<char> content_bytes(content.begin(), content.end());
        return write_sandbox_file_atomically(file, content_bytes, []() { return std::nullopt; });
    } catch (const std::exception& e) {
        return "Sandbox file write failed: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, bool> file_glob(SandboxPathResolver& resolver,
                                                    std::string_view pattern,
                                                    std::string_view base_path,
                                                    std::string_view home_mount_path,
                                                    std::optional<int> depth) {
    std::string base(base_path);
    if (base.empty()) base = std::string(home_mount_path);

    auto resolved_base = resolver.resolve_sandbox_path(base);
    std::vector<std::string> files;
    int remaining = (!depth.has_value() || *depth <= 0) ? -1 : *depth;
    resolver.walk_virtual_files(resolved_base.file, files,
                                resolved_base.physical_root.string(),
                                resolved_base.virtual_root, remaining);

    std::vector<std::string> matches = glob_match(files, pattern);
    bool truncated = matches.size() >= 1000;  // SHELL_FILE_GLOB_MAX_MATCHES
    if (truncated) matches.resize(1000);
    return {matches, truncated};
}

std::optional<GrepResult> file_grep(SandboxPathResolver& resolver,
                                    std::string_view pattern,
                                    std::string_view base_path,
                                    std::string_view home_mount_path,
                                    std::string_view file_glob) {
    try {
        std::regex regex{std::string(pattern)};
        std::string base = base_path.empty() ? std::string(home_mount_path) : std::string(base_path);
        auto resolved_base = resolver.resolve_sandbox_path(base);
        std::vector<std::string> all_files;
        resolver.walk_virtual_files(resolved_base.file, all_files,
                                    resolved_base.physical_root.string(),
                                    resolved_base.virtual_root);

        std::vector<std::string> files = file_glob.empty()
                                             ? all_files
                                             : glob_match(all_files, file_glob);

        constexpr std::size_t SHELL_FILE_GREP_MAX_MATCHES = 500;
        constexpr std::size_t SHELL_FILE_GREP_MAX_FILE_BYTES = 500 * 1024;
        constexpr std::size_t SHELL_FILE_GREP_MAX_CONTENT_CHARS = 500;

        std::vector<GrepMatch> matches;
        for (const auto& file : files) {
            try {
                std::filesystem::path resolved;
                if (file.starts_with("/")) {
                    resolved = resolver.resolve_path(file);
                } else {
                    resolved = resolver.resolve_path("/" + file);
                }
                std::error_code ec;
                if (!std::filesystem::exists(resolved, ec) ||
                    std::filesystem::file_size(resolved, ec) > SHELL_FILE_GREP_MAX_FILE_BYTES) {
                    continue;
                }

                std::ifstream input(resolved, std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());

                // Skip binary files (NUL byte heuristic)
                if (text.find('\0') != std::string::npos) continue;

                std::size_t line_number = 0;
                std::string line;
                std::istringstream lines(text);
                while (std::getline(lines, line)) {
                    ++line_number;
                    if (std::regex_search(line, regex)) {
                        matches.push_back(GrepMatch{
                            .path = file,
                            .line = static_cast<std::int32_t>(line_number),
                            .content = line.substr(0, SHELL_FILE_GREP_MAX_CONTENT_CHARS),
                        });
                        if (matches.size() >= SHELL_FILE_GREP_MAX_MATCHES) {
                            return GrepResult{std::move(matches), true};
                        }
                    }
                }
            } catch (const std::exception&) {
                // Skip unreadable files
            }
        }
        return GrepResult{std::move(matches), false};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

shell_file::ShellFileEditResult file_edit(SandboxPathResolver& resolver,
                                          std::string_view path,
                                          std::string_view old_string,
                                          std::string_view new_string,
                                          bool replace_all,
                                          std::mutex& mutation_mutex) {
    std::lock_guard<std::mutex> lock(mutation_mutex);

    constexpr std::int64_t SHELL_FILE_EDIT_MAX_BYTES = 16'777'216L;

    try {
        auto file = resolver.resolve_path(path);
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) {
            return shell_file::ShellFileEditResult{0, "", "File not found: " + std::string(path)};
        }
        if (!std::filesystem::is_regular_file(file, ec)) {
            return shell_file::ShellFileEditResult{0, "", "path is not a regular file"};
        }
        if (std::filesystem::file_size(file, ec) > SHELL_FILE_EDIT_MAX_BYTES) {
            return shell_file::ShellFileEditResult{0, "", "file exceeds 16MB edit limit"};
        }

        // Read original content
        std::ifstream input(file, std::ios::binary);
        std::string original_content((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
        if (original_content.size() > static_cast<size_t>(SHELL_FILE_EDIT_MAX_BYTES)) {
            return shell_file::ShellFileEditResult{0, "", "file exceeds 16MB edit limit"};
        }

        // Convert to UTF-16 for edit_shell_file_content
        std::u16string original_u16 = agora::core::utf16::from_utf8(original_content);
        std::u16string old_u16 = agora::core::utf16::from_utf8(old_string);
        std::u16string new_u16 = agora::core::utf16::from_utf8(new_string);

        auto [edited_opt, edit_result] =
            shell_file::edit_shell_file_content(original_u16, old_u16, new_u16, replace_all);
        if (!edited_opt) return edit_result;

        std::string edited_utf8 = agora::core::utf16::to_utf8(*edited_opt);
        std::vector<char> edited_bytes(edited_utf8.begin(), edited_utf8.end());
        if (edited_bytes.size() > static_cast<size_t>(SHELL_FILE_EDIT_MAX_BYTES)) {
            return shell_file::ShellFileEditResult{0, "", "edited file exceeds 16MB limit"};
        }

        // Check for concurrent modification
        std::string current_content;
        {
            std::ifstream current_input(file, std::ios::binary);
            current_content.assign((std::istreambuf_iterator<char>(current_input)),
                                   std::istreambuf_iterator<char>());
        }
        if (current_content != original_content) {
            return shell_file::ShellFileEditResult{
                0, "", "file changed concurrently before edit could be applied"};
        }

        auto write_error = write_sandbox_file_atomically(file, edited_bytes, []() { return std::nullopt; });
        if (write_error) {
            return shell_file::ShellFileEditResult{0, "", *write_error};
        }

        edit_result.sha256 = shell_file::shell_file_sha256(edited_utf8);
        return edit_result;
    } catch (const std::exception& e) {
        return shell_file::ShellFileEditResult{0, "", "Sandbox file edit failed: " + std::string(e.what())};
    }
}

}  // namespace file_operations

}  // namespace agora::core::sandbox