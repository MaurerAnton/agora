#include "agora/core/memory_manager.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "agora/core/json.hpp"

namespace agora::core::memory {
namespace {

using json::Kind;
using json::Value;

// ---------------------------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------------------------

/** Java File.getCanonicalPath(): resolves symlinks and "..", works for paths that don't exist. */
std::filesystem::path to_canonical(const std::filesystem::path& path) {
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) {
        // Fall back to the lexical form, matching Java's best-effort canonicalisation.
        return path.lexically_normal();
    }
    return resolved;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return "";
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool is_regular(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool dir_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

/** Kotlin File.delete(): true when the entry is gone afterwards. */
bool remove_entry(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::remove(path, error) || !path_exists(path);
}

/** Kotlin File.renameTo(): true on success, false instead of throwing. */
bool rename_entry(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    return !error;
}

/**
 * Kotlin writeText() followed by FileDescriptor.sync(): the bytes have to be on disk before the
 * metadata dance renames the temporary file into place.
 */
void write_and_sync(const std::filesystem::path& path, std::string_view content) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        throw std::runtime_error("Unable to write " + path.filename().string());
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t count = ::write(descriptor, content.data() + written, content.size() - written);
        if (count < 0) {
            const int saved = errno;
            ::close(descriptor);
            throw std::runtime_error(std::string("Unable to write ") + path.filename().string() +
                                     ": " + std::strerror(saved));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        const int saved = errno;
        ::close(descriptor);
        throw std::runtime_error(std::string("Unable to sync ") + path.filename().string() +
                                 ": " + std::strerror(saved));
    }
    ::close(descriptor);
}

// ---------------------------------------------------------------------------------------------
// kotlinx Json { prettyPrint = true } for a flat string map
// ---------------------------------------------------------------------------------------------

/** kotlinx prints four spaces per level and puts "key": value on one line. */
std::string pretty_sorted(const std::map<std::string, std::string>& values) {
    if (values.empty()) return "{}";
    std::string out = "{\n";
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) out += ",\n";
        first = false;
        out += "    ";
        out.push_back('"');
        json::escape_to(out, key);
        out.push_back('"');
        out += ": ";
        out.push_back('"');
        json::escape_to(out, value);
        out.push_back('"');
    }
    out += "\n}";
    return out;
}

/** kotlinx decodeFromString<Map<String, String>>: an object whose every value is a string. */
std::map<std::string, std::string> parse_string_map(std::string_view source) {
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        throw std::invalid_argument("Invalid metadata JSON");
    }
    std::map<std::string, std::string> out;
    for (const json::Member& member : parsed.value->members) {
        if (member.value.kind != Kind::String) {
            throw std::invalid_argument("Invalid metadata JSON");
        }
        out[member.key] = member.value.text;  // last duplicate key wins
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// DescriptionMetadataStore
// ---------------------------------------------------------------------------------------------

/**
 * Port of data/DescriptionMetadataStore.kt: reads/writes memory_meta.json through a temporary
 * file plus a backup, so a crash mid-write is recoverable rather than corrupting the catalog.
 */
class MemoryManager::MetadataStore {
public:
    explicit MetadataStore(std::filesystem::path metadata_file)
        : metadata_file_(std::move(metadata_file)),
          temporary_file_(metadata_file_.parent_path() / (metadata_file_.filename().string() + ".tmp")),
          backup_file_(metadata_file_.parent_path() / (metadata_file_.filename().string() + ".bak")) {}

    std::map<std::string, std::string> read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        recover_interrupted_write();
        if (!path_exists(metadata_file_)) return {};
        try {
            return parse_string_map(read_text(metadata_file_));
        } catch (const std::exception& error) {
            throw std::runtime_error("Unable to parse " + metadata_file_.filename().string() +
                                     "; metadata was not modified");
        }
    }

    std::string read_json() const {
        std::lock_guard<std::mutex> lock(mutex_);
        recover_interrupted_write();
        return path_exists(metadata_file_) ? read_text(metadata_file_) : std::string("{}");
    }

    void replace_json(std::string_view json_string) {
        write(parse_string_map(json_string));
    }

    void write(const std::map<std::string, std::string>& values) {
        std::lock_guard<std::mutex> lock(mutex_);
        atomic_write(pretty_sorted(values));
    }

private:
    /** Kotlin recoverInterruptedWrite(): finish or discard a write that was cut short. */
    void recover_interrupted_write() const {
        if (path_exists(backup_file_)) {
            if (path_exists(metadata_file_)) {
                remove_entry(backup_file_);
            } else if (!rename_entry(backup_file_, metadata_file_)) {
                throw std::runtime_error("Unable to restore " + metadata_file_.filename().string() +
                                         " from backup");
            }
        }
        if (path_exists(temporary_file_)) remove_entry(temporary_file_);
    }

    void atomic_write(const std::string& content) const {
        recover_interrupted_write();
        const auto parent = metadata_file_.parent_path();
        if (!dir_exists(parent)) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error || !dir_exists(parent)) {
                throw std::runtime_error("Unable to create metadata directory: " +
                                         parent.string());
            }
        }
        if (path_exists(temporary_file_) && !remove_entry(temporary_file_)) {
            throw std::runtime_error("Unable to remove stale metadata temporary file");
        }

        write_and_sync(temporary_file_, content);

        bool original_moved = false;
        try {
            if (path_exists(metadata_file_)) {
                if (path_exists(backup_file_) && !remove_entry(backup_file_)) {
                    throw std::runtime_error("Unable to remove stale metadata backup");
                }
                if (!rename_entry(metadata_file_, backup_file_)) {
                    throw std::runtime_error("Unable to stage existing " +
                                             metadata_file_.filename().string());
                }
                original_moved = true;
            }
            if (!rename_entry(temporary_file_, metadata_file_)) {
                throw std::runtime_error("Unable to commit " + metadata_file_.filename().string());
            }
            if (path_exists(backup_file_)) remove_entry(backup_file_);
        } catch (...) {
            if (original_moved && !path_exists(metadata_file_)) {
                rename_entry(backup_file_, metadata_file_);
            }
            if (path_exists(temporary_file_)) remove_entry(temporary_file_);
            throw;
        }
        if (path_exists(temporary_file_)) remove_entry(temporary_file_);
    }

    mutable std::mutex mutex_;
    std::filesystem::path metadata_file_;
    std::filesystem::path temporary_file_;
    std::filesystem::path backup_file_;
};

// ---------------------------------------------------------------------------------------------
// MemoryManager
// ---------------------------------------------------------------------------------------------

MemoryManager::MemoryManager(std::string files_dir)
    : memory_dir_(std::filesystem::path(files_dir) / "memory_db"),
      active_memory_(std::filesystem::path(files_dir) / "active_memory.md"),
      meta_file_(memory_dir_ / "memory_meta.json"),
      metadata_(std::make_unique<MetadataStore>(meta_file_)) {
    std::error_code error;
    std::filesystem::create_directories(memory_dir_, error);
}

MemoryManager::~MemoryManager() = default;

std::string MemoryManager::get_active_memory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return get_active_memory_unlocked();
}

std::string MemoryManager::get_active_memory_unlocked() const {
    return path_exists(active_memory_) ? read_text(active_memory_) : std::string();
}

std::string MemoryManager::update_active_memory(std::string_view content, std::string_view mode,
                                                const std::optional<std::string>& old_string,
                                                const std::optional<std::string>& new_string) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result;

    if (mode == "append") {
        std::ofstream stream(active_memory_, std::ios::app | std::ios::binary);
        stream << "\n" << content;
        result = "Appended to active memory.";
    } else if (mode == "prepend") {
        const std::string existing = get_active_memory_unlocked();
        write_text(active_memory_, std::string(content) + "\n" + existing);
        result = "Prepended to active memory.";
    } else if (mode == "patch") {
        if (!old_string || old_string->empty()) {
            throw std::invalid_argument("old_string is required for patch mode");
        }
        const std::string existing = get_active_memory_unlocked();
        std::size_t matches = 0;
        for (std::size_t at = existing.find(*old_string); at != std::string::npos;
             at = existing.find(*old_string, at + old_string->size())) {
            ++matches;
        }
        if (matches != 1) {
            if (matches == 0) {
                throw std::invalid_argument("old_string not found in active memory");
            }
            throw std::invalid_argument("old_string matches " + std::to_string(matches) +
                                        " times in active memory; it must be unique");
        }
        std::string patched = existing;
        patched.replace(existing.find(*old_string), old_string->size(), new_string.value_or(""));
        write_text(active_memory_, patched);
        result = "Active memory patched.";
    } else {
        write_text(active_memory_, content);
        result = "Active memory updated.";
    }

    ++active_memory_revision_;
    return result;
}

std::string MemoryManager::get_description(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto resolved = resolve_file(name);
    if (!path_exists(resolved)) return "";
    const auto values = metadata_->read();
    const auto found = values.find(relative_path(resolved));
    return found == values.end() ? std::string() : found->second;
}

void MemoryManager::set_description(std::string_view name, std::string_view description) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto resolved = resolve_file(name);
    if (!path_exists(resolved)) {
        throw std::invalid_argument("File not found: " + std::string(name));
    }
    auto values = metadata_->read();
    const auto original = values;
    const std::string key = relative_path(resolved);
    const bool blank = std::all_of(description.begin(), description.end(),
                                   [](unsigned char c) { return std::isspace(c) != 0; });
    if (blank) {
        values.erase(key);
    } else {
        values[key] = std::string(description);
    }
    if (values != original) {
        metadata_->write(values);
        ++catalog_revision_;
    }
}

std::vector<MemoryManager::MemoryFileInfo> MemoryManager::list_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto values = metadata_->read();
    std::vector<MemoryFileInfo> out;

    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(memory_dir_, error), end;
         it != end; it.increment(error)) {
        if (error) break;
        if (!is_regular(it->path())) continue;
        if (it->path().extension() != ".md") continue;
        const std::string key = relative_path(it->path());
        const auto found = values.find(key);
        out.push_back({key, found == values.end() ? std::string() : found->second});
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const MemoryFileInfo& a, const MemoryFileInfo& b) { return a.name < b.name; });
    return out;
}

std::string MemoryManager::get_meta_json() const { return metadata_->read_json(); }

void MemoryManager::save_meta_json(std::string_view json_string) {
    metadata_->replace_json(json_string);
    std::lock_guard<std::mutex> lock(mutex_);
    ++catalog_revision_;
}

std::string MemoryManager::read_file(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto resolved = resolve_file(name);
    if (!path_exists(resolved)) {
        throw std::invalid_argument("File not found: " + std::string(name));
    }
    return read_text(resolved);
}

std::string MemoryManager::create_file(std::string_view name, std::string_view content,
                                       std::string_view description) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = resolve_file(name);
    if (path_exists(file)) {
        throw std::invalid_argument("File already exists: " + relative_path(file));
    }

    const bool with_description =
        !std::all_of(description.begin(), description.end(),
                     [](unsigned char c) { return std::isspace(c) != 0; });
    std::map<std::string, std::string> values;
    if (with_description) values = metadata_->read();

    try {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        write_text(file, content);
        if (with_description) {
            values[relative_path(file)] = std::string(description);
            metadata_->write(values);
        }
    } catch (...) {
        if (path_exists(file)) remove_entry(file);
        throw;
    }

    ++catalog_revision_;
    return "Created " + relative_path(file);
}

std::string MemoryManager::edit_file(std::string_view name,
                                     const std::optional<std::string>& content,
                                     const std::optional<std::string>& new_name,
                                     const std::optional<std::string>& description,
                                     const std::optional<std::string>& old_string,
                                     const std::optional<std::string>& new_string) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = resolve_file(name);
    if (!path_exists(file)) {
        throw std::invalid_argument("File not found: " + std::string(name));
    }
    if (content && old_string) {
        throw std::invalid_argument("content and old_string are mutually exclusive");
    }
    if (!content && !old_string && !new_name && !description) {
        throw std::invalid_argument("At least one edit must be provided");
    }

    std::optional<std::filesystem::path> rename_target;
    if (new_name) {
        auto candidate = resolve_file(*new_name);
        if (to_canonical(candidate) != to_canonical(file)) rename_target = candidate;
    }
    if (rename_target && path_exists(*rename_target)) {
        throw std::invalid_argument("Target file already exists: " +
                                    rename_target->filename().string());
    }

    std::optional<std::string> original_content;
    if (content || old_string) original_content = read_text(file);

    std::optional<std::string> replacement_content;
    if (old_string) {
        if (old_string->empty()) {
            throw std::invalid_argument("old_string must not be empty");
        }
        const std::string& existing = *original_content;
        std::size_t matches = 0;
        for (std::size_t at = existing.find(*old_string); at != std::string::npos;
             at = existing.find(*old_string, at + old_string->size())) {
            ++matches;
        }
        if (matches != 1) {
            const std::string label(file.filename().string());
            if (matches == 0) {
                throw std::invalid_argument("old_string not found in " + label);
            }
            throw std::invalid_argument("old_string matches " + std::to_string(matches) +
                                        " times in " + label + "; it must be unique");
        }
        std::string patched = existing;
        patched.replace(existing.find(*old_string), old_string->size(), new_string.value_or(""));
        replacement_content = patched;
    } else if (content) {
        replacement_content = *content;
    }

    const bool touches_metadata = rename_target.has_value() || description.has_value();
    std::optional<std::map<std::string, std::string>> values;
    std::map<std::string, std::string> original_values;
    if (touches_metadata) {
        values = metadata_->read();
        original_values = *values;
    }

    const std::string source_key = relative_path(file);
    const std::string target_key =
        rename_target ? relative_path(*rename_target) : source_key;
    if (values) {
        if (rename_target) {
            const auto found = values->find(source_key);
            if (found != values->end()) {
                (*values)[target_key] = found->second;
                values->erase(found);
            }
        }
        if (description) {
            const bool blank = std::all_of(description->begin(), description->end(),
                                           [](unsigned char c) { return std::isspace(c) != 0; });
            if (blank) {
                values->erase(target_key);
            } else {
                (*values)[target_key] = *description;
            }
        }
    }

    // Kotlin: `values != null && values != originalValues`; an edit that changes nothing reports
    // "No changes made." instead of claiming an update.
    const bool metadata_changed = values.has_value() && *values != original_values;

    std::filesystem::path target = file;
    bool content_touched = false;
    bool renamed = false;
    try {
        if (replacement_content) {
            content_touched = true;
            write_text(file, *replacement_content);
        }
        if (rename_target) {
            std::error_code error;
            std::filesystem::create_directories(rename_target->parent_path(), error);
            if (!rename_entry(file, *rename_target)) {
                throw std::runtime_error("Unable to rename " + file.filename().string());
            }
            target = *rename_target;
            renamed = true;
        }
        if (metadata_changed) metadata_->write(*values);
    } catch (...) {
        if (content_touched && original_content) write_text(target, *original_content);
        if (renamed) rename_entry(target, file);
        throw;
    }

    const bool changed = replacement_content.has_value() || renamed || metadata_changed;
    if (changed) {
        if (renamed) prune_empty_parents(file.parent_path());
        ++catalog_revision_;
    }
    return changed ? "Updated " + target.filename().string() : std::string("No changes made.");
}

std::string MemoryManager::delete_file(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = resolve_file(name);
    if (!path_exists(file)) {
        throw std::invalid_argument("File not found: " + std::string(name));
    }
    const auto values = metadata_->read();
    const std::string key = relative_path(file);
    const std::string content = read_text(file);
    if (!remove_entry(file)) {
        throw std::invalid_argument("Unable to delete " + file.filename().string());
    }
    if (values.find(key) != values.end()) {
        try {
            auto updated = values;
            updated.erase(key);
            metadata_->write(updated);
        } catch (...) {
            write_text(file, content);
            throw;
        }
    }
    ++catalog_revision_;
    prune_empty_parents(file.parent_path());
    return "Deleted " + file.filename().string();
}

std::int64_t MemoryManager::active_memory_revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_memory_revision_;
}

std::int64_t MemoryManager::catalog_revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return catalog_revision_;
}

std::string MemoryManager::relative_path(const std::filesystem::path& file) const {
    const auto relative = to_canonical(file).lexically_relative(to_canonical(memory_dir_));
    return relative.generic_string();
}

/** Kotlin File.listFiles(): continue upward while the directory is empty, never removing the root. */
void MemoryManager::prune_empty_parents(const std::filesystem::path& dir) const {
    const auto root = to_canonical(memory_dir_);
    std::filesystem::path current = to_canonical(dir);
    while (current != root && dir_exists(current)) {
        std::error_code error;
        const bool empty = std::filesystem::directory_iterator(current, error) ==
                           std::filesystem::directory_iterator();
        if (error || !empty) break;
        if (!remove_entry(current)) return;
        current = current.parent_path();
    }
}

std::filesystem::path MemoryManager::resolve_file(std::string_view name) const {
    std::string trimmed(name);
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    std::replace(trimmed.begin(), trimmed.end(), '\\', '/');
    while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    if (trimmed.empty() || trimmed == ".") {
        throw std::invalid_argument("Invalid file name: " + std::string(name));
    }

    std::vector<std::string> segments;
    std::string segment;
    for (char c : trimmed) {
        if (c == '/') {
            if (!segment.empty() && segment != ".") segments.push_back(segment);
            segment.clear();
        } else {
            segment.push_back(c);
        }
    }
    if (!segment.empty() && segment != ".") segments.push_back(segment);

    if (segments.empty() ||
        std::any_of(segments.begin(), segments.end(),
                    [](const std::string& part) { return part == ".."; })) {
        throw std::invalid_argument("Invalid file name: " + std::string(name));
    }

    const auto ends_with_md = [](std::string_view part) {
        return part.size() >= 3 &&
               std::equal(part.end() - 3, part.end(), ".md",
                          [](char a, char b) { return std::tolower(a) == b; });
    };
    if (!ends_with_md(segments.back())) segments.back() += ".md";

    std::filesystem::path file = memory_dir_;
    for (const auto& part : segments) file /= part;

    const auto canonical_directory = to_canonical(memory_dir_);
    const auto canonical_file = to_canonical(file);
    const auto directory_text = canonical_directory.generic_string();
    const auto file_text = canonical_file.generic_string();
    if (canonical_file != canonical_directory &&
        (file_text.size() <= directory_text.size() + 1 ||
         file_text.compare(0, directory_text.size() + 1, directory_text + "/") != 0)) {
        throw std::invalid_argument("Invalid file name: " + std::string(name));
    }
    return canonical_file;
}

void MemoryManager::write_text(const std::filesystem::path& path, std::string_view content) {
    write_and_sync(path, content);
}

}  // namespace agora::core::memory
