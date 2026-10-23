#include "agora/core/skill_manager.hpp"

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

namespace agora::core::skill {
namespace {

using json::Kind;

// ---------------------------------------------------------------------------------------------
// File helpers (Java File semantics over std::filesystem, as in memory_manager.cpp)
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

// ---------------------------------------------------------------------------------------------
// Kotlin String helpers
// ---------------------------------------------------------------------------------------------

/** Kotlin String.isNotBlank()/isBlank() over ASCII whitespace (the pinned corpus is ASCII). */
bool is_blank(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isspace(c) != 0; });
}

/**
 * Kotlin String.endsWith(".md") — used by both listFiles' `extension == "md"` (extension is
 * everything after the last dot) and resolveFile's ".md" rule; the two are equivalent because
 * "everything after the last dot equals md" and "the last three characters are .md" coincide.
 */
bool ends_with_md(std::string_view name) {
    return name.size() >= 3 && name.compare(name.size() - 3, 3, ".md") == 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// DescriptionMetadataStore
// ---------------------------------------------------------------------------------------------

/**
 * Port of data/DescriptionMetadataStore.kt: reads/writes skill_meta.json through a temporary
 * file plus a backup, so a crash mid-write is recoverable rather than corrupting the catalog.
 */
class SkillManager::MetadataStore {
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
        } catch (const std::exception&) {
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
// SkillManager
// ---------------------------------------------------------------------------------------------

SkillManager::SkillManager(std::string files_dir)
    : skill_dir_(std::filesystem::path(files_dir) / "skill_db"),
      meta_file_(skill_dir_ / "skill_meta.json"),
      metadata_(std::make_unique<MetadataStore>(meta_file_)) {
    std::error_code error;
    std::filesystem::create_directories(skill_dir_, error);
}

SkillManager::~SkillManager() = default;

std::filesystem::path SkillManager::resolve_file(std::string_view name) const {
    std::string sanitized(name);
    std::replace(sanitized.begin(), sanitized.end(), '/', '_');
    std::replace(sanitized.begin(), sanitized.end(), '\\', '_');
    if (!ends_with_md(sanitized)) sanitized += ".md";

    const auto canonical_directory = to_canonical(skill_dir_);
    const auto canonical_file = to_canonical(skill_dir_ / sanitized);
    // Kotlin require(canonicalFile.parentFile == canonicalDirectory): the skill catalog is flat,
    // so a resolved file can never leave skill_db.
    if (canonical_file.parent_path() != canonical_directory) {
        throw std::invalid_argument("Invalid file name: " + std::string(name));
    }
    return canonical_file;
}

std::vector<SkillManager::SkillFileInfo> SkillManager::list_files_unlocked() const {
    const auto values = metadata_->read();
    std::vector<SkillFileInfo> out;

    std::error_code error;
    for (std::filesystem::directory_iterator it(skill_dir_, error), end;
         !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (!ends_with_md(name)) continue;
        const auto found = values.find(name);
        out.push_back({name, found == values.end() ? std::string() : found->second});
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const SkillFileInfo& a, const SkillFileInfo& b) { return a.name < b.name; });
    return out;
}

std::vector<SkillManager::SkillFileInfo> SkillManager::list_files() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_files_unlocked();
}

std::string SkillManager::catalog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto files = list_files_unlocked();
    if (files.empty()) return "";

    std::string out = "<available_skills>\n";
    out += "Use the skill file tools to read a relevant skill before following it. "
           "Only names and descriptions are preloaded.\n";
    for (const auto& file : files) {
        out += "- ";
        out += file.name;
        if (!is_blank(file.description)) {
            std::string description = file.description;
            std::replace(description.begin(), description.end(), '\n', ' ');
            const auto first = std::find_if(description.begin(), description.end(),
                                            [](unsigned char c) { return !std::isspace(c); });
            const auto last = std::find_if(description.rbegin(), description.rend(),
                                           [](unsigned char c) { return !std::isspace(c); })
                                  .base();
            out += ": " + description.substr(static_cast<std::size_t>(first - description.begin()),
                                             static_cast<std::size_t>(last - first));
        }
        out += "\n";
    }
    out += "</available_skills>";
    return out;
}

std::string SkillManager::get_description(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto resolved = resolve_file(name);
    if (!path_exists(resolved)) return "";
    const auto values = metadata_->read();
    const auto found = values.find(resolved.filename().string());
    return found == values.end() ? std::string() : found->second;
}

std::string SkillManager::get_meta_json() const { return metadata_->read_json(); }

void SkillManager::save_meta_json(std::string_view json_string) {
    metadata_->replace_json(json_string);
    std::lock_guard<std::mutex> lock(mutex_);
    ++catalog_revision_;
}

std::string SkillManager::read_file(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto resolved = resolve_file(name);
    if (!path_exists(resolved)) {
        throw std::invalid_argument("File not found: " + std::string(name));
    }
    return read_text(resolved);
}

std::string SkillManager::create_file(std::string_view name, std::string_view content,
                                      std::string_view description) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = resolve_file(name);
    if (path_exists(file)) {
        throw std::invalid_argument("File already exists: " + file.filename().string());
    }

    const bool with_description = !is_blank(description);
    std::map<std::string, std::string> values;
    if (with_description) values = metadata_->read();

    try {
        write_and_sync(file, content);
        if (with_description) {
            values[file.filename().string()] = std::string(description);
            metadata_->write(values);
        }
    } catch (...) {
        if (path_exists(file)) remove_entry(file);
        throw;
    }

    ++catalog_revision_;
    return "Created " + file.filename().string();
}

std::string SkillManager::edit_file(std::string_view name,
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
        if (candidate.filename() != file.filename()) rename_target = candidate;
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

    const std::string source_key = file.filename().string();
    const std::string target_key =
        rename_target ? rename_target->filename().string() : source_key;
    if (values) {
        if (rename_target) {
            const auto found = values->find(source_key);
            if (found != values->end()) {
                (*values)[target_key] = found->second;
                values->erase(found);
            }
        }
        if (description) {
            if (is_blank(*description)) {
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
            write_and_sync(file, *replacement_content);
        }
        if (rename_target) {
            if (!rename_entry(file, *rename_target)) {
                throw std::runtime_error("Unable to rename " + file.filename().string());
            }
            target = *rename_target;
            renamed = true;
        }
        if (metadata_changed) metadata_->write(*values);
    } catch (...) {
        if (content_touched && original_content) write_and_sync(target, *original_content);
        if (renamed) rename_entry(target, file);
        throw;
    }

    const bool changed = replacement_content.has_value() || renamed || metadata_changed;
    if (changed) ++catalog_revision_;
    return changed ? "Updated " + target.filename().string() : std::string("No changes made.");
}

std::string SkillManager::delete_file(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = resolve_file(name);
    if (!path_exists(file)) {
        throw std::invalid_argument("File not found: " + std::string(name));
    }
    const auto values = metadata_->read();
    const std::string key = file.filename().string();
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
            write_and_sync(file, content);
            throw;
        }
    }
    ++catalog_revision_;
    return "Deleted " + file.filename().string();
}

std::int64_t SkillManager::catalog_revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return catalog_revision_;
}

}  // namespace agora::core::skill
