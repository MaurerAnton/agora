#include "agora/core/alpine_package_metadata_store.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace agora::core::sandbox {

namespace {

constexpr const char* kDefaultBaseWorld[] = {
    "alpine-baselayout", "alpine-keys", "apk-tools", "busybox", "libc-utils",
};

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return out;
    std::ifstream input(path, std::ios::binary);
    if (!input) return out;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (text.empty()) return out;
    // Java readLines semantics: terminators split, a trailing terminator adds no empty line.
    if (text.back() == '\n') text.pop_back();
    else if (text.back() == '\r') text.pop_back();
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

void write_lines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    for (const std::string& line : lines) out << line << "\n";
}

std::string trim_copy(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

/** LinkedHashSet push: drop duplicates, keep first-occurrence order. */
void append_unique(std::vector<std::string>& out, const std::string& value) {
    if (std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
}

bool in_list(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

std::string world_package_name(const std::string& line) {
    std::string cleaned = trim_copy(line);
    if (!cleaned.empty() && cleaned.front() == '!') cleaned.erase(0, 1);
    const auto end = cleaned.find_first_of("@<>=~");
    return end == std::string::npos ? cleaned : cleaned.substr(0, end);
}

AlpinePackageMetadataStore::AlpinePackageMetadataStore(std::filesystem::path rootfs_dir)
    : rootfs_dir_(std::move(rootfs_dir)) {}

const std::vector<std::string>& AlpinePackageMetadataStore::default_base_world() {
    static const std::vector<std::string> world(std::begin(kDefaultBaseWorld),
                                                std::end(kDefaultBaseWorld));
    return world;
}

std::string AlpinePackageMetadataStore::sanitize_package_name(const std::string& package_name) {
    const std::string trimmed = trim_copy(package_name);
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9+_.:-]*$");
    if (!std::regex_match(trimmed, pattern)) {
        throw std::invalid_argument("Invalid package name: " + package_name);
    }
    return trimmed;
}

bool AlpinePackageMetadataStore::contains(const InstalledVersions& versions,
                                          const std::string& name) {
    return std::any_of(versions.begin(), versions.end(),
                       [&](const auto& entry) { return entry.first == name; });
}

AlpinePackageMetadataStore::InstalledVersions AlpinePackageMetadataStore::read_installed_versions() {
    InstalledVersions installed;
    std::string name;
    std::string version;
    // Kotlin: locals `name`/`version`, flush on blank and once at the end, `installed[name] = v`
    // semantics (a repeated name overwrites the value but keeps its first position).
    const auto flush = [&] {
        if (!name.empty()) {
            const auto it = std::find_if(installed.begin(), installed.end(),
                                         [&](const auto& e) { return e.first == name; });
            if (it != installed.end()) {
                it->second = version;
            } else {
                installed.emplace_back(name, version);
            }
        }
        name.clear();
        version.clear();
    };
    for (const std::string& line : read_lines(installed_db_file())) {
        if (line.rfind("P:", 0) == 0) {
            name = trim_copy(line.substr(2));
        } else if (line.rfind("V:", 0) == 0) {
            version = trim_copy(line.substr(2));
        } else if (trim_copy(line).empty()) {
            flush();
        }
    }
    flush();
    return installed;
}

std::vector<std::string> AlpinePackageMetadataStore::read_world_lines() {
    std::vector<std::string> world;
    for (const std::string& line : read_lines(world_file())) {
        const std::string trimmed = trim_copy(line);
        if (!trimmed.empty()) append_unique(world, trimmed);
    }
    return world;
}

void AlpinePackageMetadataStore::write_world_lines(const std::vector<std::string>& lines) {
    write_lines(world_file(), lines);
}

void AlpinePackageMetadataStore::capture_base_world(bool force) {
    std::error_code ec;
    std::filesystem::create_directories(metadata_dir(), ec);
    if (!force && std::filesystem::exists(base_world_file(), ec)) return;

    const std::vector<std::string> current = read_world_lines();
    const std::vector<std::string> installed_keys = [&] {
        std::vector<std::string> keys;
        for (const auto& entry : read_installed_versions()) keys.push_back(entry.first);
        return keys;
    }();

    std::vector<std::string> inferred_base;
    for (const std::string& line : current) {
        const std::string name = world_package_name(line);
        if (in_list(default_base_world(), name) && in_list(installed_keys, name)) {
            append_unique(inferred_base, line);
        }
    }
    std::vector<std::string> fallback_base;
    for (const std::string& name : default_base_world()) {
        if (in_list(installed_keys, name)) fallback_base.push_back(name);
    }

    std::vector<std::string> base;
    if (force && !current.empty()) {
        base = current;
    } else if (force) {
        base = fallback_base.empty() ? default_base_world() : fallback_base;
    } else if (!inferred_base.empty()) {
        base = inferred_base;
    } else {
        base = current;
    }
    write_lines(base_world_file(), base);
}

std::vector<std::string> AlpinePackageMetadataStore::read_base_world() {
    capture_base_world(false);
    std::vector<std::string> base;
    for (const std::string& line : read_lines(base_world_file())) {
        const std::string trimmed = trim_copy(line);
        if (!trimmed.empty()) append_unique(base, trimmed);
    }
    return base;
}

bool AlpinePackageMetadataStore::is_base_package(const std::string& package_name) {
    for (const std::string& line : read_base_world()) {
        if (world_package_name(line) == package_name) return true;
    }
    return false;
}

std::vector<std::string> AlpinePackageMetadataStore::read_explicit_packages() {
    std::vector<std::string> explicit_packages;
    for (const std::string& line : read_lines(explicit_packages_file())) {
        try {
            append_unique(explicit_packages, sanitize_package_name(line));
        } catch (const std::invalid_argument&) {
            // Kotlin runCatching drops entries that no longer sanitize.
        }
    }
    return explicit_packages;
}

void AlpinePackageMetadataStore::write_explicit_packages(const std::vector<std::string>& packages) {
    std::error_code ec;
    std::filesystem::create_directories(metadata_dir(), ec);
    std::vector<std::string> clean;
    for (const std::string& package : packages) {
        try {
            append_unique(clean, sanitize_package_name(package));
        } catch (const std::invalid_argument&) {
        }
    }
    write_lines(explicit_packages_file(), clean);
}

void AlpinePackageMetadataStore::ensure_package_metadata() {
    if (!std::filesystem::is_directory(rootfs_dir_)) return;
    std::error_code ec;
    std::filesystem::create_directories(metadata_dir(), ec);
    capture_base_world(false);
    if (!std::filesystem::exists(explicit_packages_file(), ec)) {
        std::vector<std::string> base_names;
        for (const std::string& line : read_base_world()) {
            base_names.push_back(world_package_name(line));
        }
        std::vector<std::string> migrated;
        for (const std::string& line : read_world_lines()) {
            const std::string name = world_package_name(line);
            if (in_list(base_names, name)) continue;
            try {
                append_unique(migrated, sanitize_package_name(name));
            } catch (const std::invalid_argument&) {
            }
        }
        write_explicit_packages(migrated);
    }
    normalize_world();
}

void AlpinePackageMetadataStore::normalize_world() {
    normalize_world(read_explicit_packages());
}

void AlpinePackageMetadataStore::normalize_world(const std::vector<std::string>& explicit_packages) {
    std::error_code ec;
    std::filesystem::create_directories(metadata_dir(), ec);
    const std::vector<std::string> base = read_base_world();
    std::vector<std::string> base_names;
    for (const std::string& line : base) base_names.push_back(world_package_name(line));

    std::vector<std::string> normalized;
    for (const std::string& line : base) append_unique(normalized, line);
    for (const std::string& package : explicit_packages) {
        std::string clean;
        try {
            clean = sanitize_package_name(package);
        } catch (const std::invalid_argument&) {
            continue;
        }
        if (in_list(base_names, clean)) continue;
        append_unique(normalized, clean);
    }
    write_world_lines(normalized);
}

void AlpinePackageMetadataStore::add_explicit_package(const std::string& package_name) {
    const std::string name = sanitize_package_name(package_name);
    ensure_package_metadata();
    std::vector<std::string> next = read_explicit_packages();
    append_unique(next, name);
    write_explicit_packages(next);
    normalize_world(next);
}

}  // namespace agora::core::sandbox