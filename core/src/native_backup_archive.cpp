#include "agora/core/native_backup_archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <utility>

#include "agora/core/native_backup_format.hpp"

namespace agora::core::native_backup_archive {
namespace {

namespace format = agora::core::native_backup;

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

/** commons ZipArchiveEntry normalizes raw backslashes to '/' in getName(). */
std::string normalize_name(std::string_view raw) {
    std::string name(raw);
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}


/** java.lang.Math.addExact over non-negative operands. */
std::int64_t checked_add(std::int64_t left, std::int64_t right, std::string_view message) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        throw IOException(std::string(message));
    }
    return left + right;
}

/** The pinned storage guard: `Insufficient storage for <path>: N required, M available`. */
void ensure_available(std::int64_t required, std::int64_t available,
                      const std::filesystem::path& destination) {
    if (required > available) {
        throw IOException("Insufficient storage for " + destination.string() + ": " +
                          std::to_string(required) + " bytes required, " +
                          std::to_string(available) + " available");
    }
}

std::uint16_t read_u16(const std::string& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) |
                                      (static_cast<unsigned char>(bytes[at + 1]) << 8));
}

std::uint32_t read_u32(const std::string& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

struct LocalHeader {
    bool ok = false;
    std::uint16_t flags = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t compressed_size = 0;
};

/**
 * commons resolveLocalFileHeaderData: read the local header for every entry while the archive
 * opens. An unreadable header is the same failure commons wraps as its channel-read error.
 */
LocalHeader read_local_header(const std::string& bytes, std::uint64_t local_offset) {
    LocalHeader info;
    if (local_offset > bytes.size() || bytes.size() - local_offset < 30) return info;
    const std::size_t at = static_cast<std::size_t>(local_offset);
    if (read_u32(bytes, at) != 0x04034B50u) return info;
    info.flags = read_u16(bytes, at + 6);
    const std::uint32_t name_length = read_u16(bytes, at + 26);
    const std::uint32_t extra_length = read_u16(bytes, at + 28);
    const std::uint32_t compressed = read_u32(bytes, at + 18);
    if (compressed == 0xFFFFFFFFu) return info;  // local ZIP64: never in the pinned corpus
    const std::uint64_t data_offset =
        local_offset + 30 + static_cast<std::uint64_t>(name_length) + extra_length;
    if (data_offset > bytes.size()) return info;
    info.data_offset = data_offset;
    info.compressed_size = compressed;
    info.ok = true;
    return info;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Entry predicates (NativeBackupFormat.kt)
// ---------------------------------------------------------------------------------------------

bool is_streamed_payload(std::string_view name) {
    return name == format::kConversationsEntry || name == format::kTasksEntry ||
           starts_with(name, format::kConversationEntryPrefix);
}

bool is_conversation_resource(std::string_view name) {
    return starts_with(name, format::kImageMediaPrefix) ||
           starts_with(name, format::kVideoMediaPrefix) ||
           starts_with(name, format::kDraftMediaPrefix) || starts_with(name, "images/") ||
           starts_with(name, "videos/");
}

bool is_legacy_custom_font(std::string_view name) {
    constexpr std::string_view kPrefix = "custom_font/";
    if (!starts_with(name, kPrefix)) return false;
    return name.substr(kPrefix.size()).find('/') == std::string_view::npos;
}

bool is_resource_entry(std::string_view name) {
    return is_conversation_resource(name) || is_legacy_custom_font(name);
}

namespace {

bool is_in_memory_metadata(std::string_view name) {
    return !is_resource_entry(name) && !is_streamed_payload(name);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// CheckedEntryStream
// ---------------------------------------------------------------------------------------------

EntryStream::EntryStream(std::string name, std::function<std::string()> loader,
                         std::int64_t declared_size, std::uint32_t expected_crc)
    : name_(std::move(name)),
      loader_(std::move(loader)),
      declared_size_(declared_size),
      expected_crc_(expected_crc) {}

void EntryStream::ensure_loaded() {
    if (loaded_) return;
    content_ = loader_();
    loaded_ = true;
}

long long EntryStream::read(char* buffer, long long count) {
    if (count <= 0) return 0;
    ensure_loaded();
    if (cursor_ >= content_.size()) {
        verify();
        return -1;
    }
    const auto take = static_cast<long long>(
        std::min<std::size_t>(static_cast<std::size_t>(count), content_.size() - cursor_));
    total_ += take;
    if (declared_size_ >= 0 && total_ > declared_size_) {
        throw IOException(name_ + " size mismatch: declared " + std::to_string(declared_size_) +
                          " bytes, read more");
    }
    cursor_ += static_cast<std::size_t>(take);
    std::memcpy(buffer, content_.data() + cursor_ - take, static_cast<std::size_t>(take));
    return take;
}

long long EntryStream::skip(long long count) {
    if (count <= 0) return 0;
    const auto buffer_size = static_cast<long long>(std::min<long long>(count, 8192));
    std::vector<char> buffer(static_cast<std::size_t>(std::max<long long>(buffer_size, 1)));
    long long skipped = 0;
    while (skipped < count) {
        const long long want =
            std::min(static_cast<long long>(buffer.size()), count - skipped);
        const long long got = read(buffer.data(), want);
        if (got < 0) break;
        skipped += got;
    }
    return skipped;
}

void EntryStream::read_to_end() {
    char buffer[8192];
    while (read(buffer, sizeof(buffer)) >= 0) {
    }
}

void EntryStream::close() {
    // Kotlin: closing only closes; verification happens when the data is read to its end.
}

void EntryStream::verify() {
    if (verified_) return;
    verified_ = true;
    if (declared_size_ >= 0 && total_ != declared_size_) {
        throw IOException(name_ + " size mismatch: declared " + std::to_string(declared_size_) +
                          " bytes, read " + std::to_string(total_));
    }
    if (agora::core::zip::crc32(content_) != expected_crc_) {
        throw IOException(name_ + " CRC mismatch");
    }
}

// ---------------------------------------------------------------------------------------------
// NativeBackupArchive
// ---------------------------------------------------------------------------------------------

struct NativeBackupArchive::Item {
    zip::CentralEntry entry;
    std::string name;       // commons-normalized display name
    std::string raw_name;   // raw central-directory bytes
    bool directory = false;
    LocalHeader local;      // resolved while opening
};

struct NativeBackupArchive::State {
    State(std::string source, const OpenOptions& options)
        : bytes(std::move(source)),
          reader(zip::Reader::from_bytes(bytes)),
          metadata_limit_bytes(options.metadata_limit_bytes),
          source_label(options.source_label) {}

    std::string bytes;
    zip::Reader reader;
    std::vector<Item> items;
    std::vector<std::size_t> file_order;  // non-directory indexes, central order
    std::int64_t metadata_limit_bytes;
    std::string source_label;
};

std::string NativeBackupArchive::read_content(State& state, const Item& item) {
    if (item.entry.method == 0) {
        // STORED: ZipFile streams exactly the local-header framing, not the central record.
        if ((item.local.flags & 0x08) != 0) {
            throw IOException("only DEFLATED entries can have EXT descriptor");
        }
        const std::uint64_t data = item.local.data_offset;
        const std::uint64_t count = item.local.compressed_size;
        if (data > state.bytes.size() || state.bytes.size() - data < count) {
            throw IOException("Error reading Zip content from " + state.source_label);
        }
        return state.bytes.substr(static_cast<std::size_t>(data),
                                  static_cast<std::size_t>(count));
    }
    if (item.entry.method != 8) {
        throw IOException("Error reading Zip content from " + state.source_label);
    }
    try {
        return state.reader.read_at_unchecked(item.entry.local_offset, item.entry.size);
    } catch (const zip::ZipException& error) {
        // The inflater's own failure text (e.g. "invalid stored block lengths"), exactly what
        // commons propagates from getInputStream.
        throw IOException(error.what());
    }
}

NativeBackupArchive NativeBackupArchive::core_open(std::string bytes,
                                                   const OpenOptions& options) {
    NativeBackupArchive archive;
    try {
        archive.state_ = std::make_shared<State>(std::move(bytes), options);
    } catch (const zip::ZipException&) {
        throw IOException("Error reading Zip content from " + options.source_label);
    }
    State& state = *archive.state_;

    // Commons' constructor pass: central directory plus one local-header resolution per entry.
    std::vector<zip::CentralEntry> central;
    try {
        central = state.reader.central_entries();
    } catch (const zip::ZipException&) {
        throw IOException("Error reading Zip content from " + options.source_label);
    }

    // validateArchive: require(...) first, Kotlin entry validation, then metadata verification.
    if (options.metadata_limit_bytes < 0) {
        throw IOException("Invalid backup archive");
    }

    std::vector<std::string> canonical_names;
    std::unordered_set<std::string> canonical_seen;
    std::int64_t declared_metadata_bytes = 0;

    state.items.reserve(central.size());
    for (const zip::CentralEntry& entry : central) {
        Item item;
        item.entry = entry;
        item.raw_name = entry.name;
        item.name = normalize_name(entry.name);
        item.directory = !item.name.empty() && item.name.back() == '/';
        item.local = read_local_header(state.bytes, entry.local_offset);
        if (!item.local.ok) {
            throw IOException("Error reading Zip content from " + state.source_label);
        }
        const std::size_t item_index = state.items.size();
        state.items.push_back(item);
        const Item& stored = state.items.back();
        const std::string& name = stored.name;

        if (stored.raw_name.find('\\') != std::string::npos) {
            throw IOException("Ambiguous ZIP entry path: " + name);
        }
        if (name.empty() || name.front() == '/' || name.front() == '\\') {
            throw IOException("Unsafe ZIP entry path: " + name);
        }
        if (name.find('\\') != std::string::npos ||
            (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
             name[1] == ':')) {
            throw IOException("Ambiguous ZIP entry path: " + name);
        }
        std::string canonical = name;
        if (stored.directory) canonical.pop_back();
        if (canonical.empty()) throw IOException("Unsafe ZIP entry path: " + name);
        std::size_t at = 0;
        while (at <= canonical.size()) {
            const std::size_t slash = canonical.find('/', at);
            const std::string_view segment =
                std::string_view(canonical).substr(at, slash == std::string::npos
                                                           ? std::string_view::npos
                                                           : slash - at);
            if (segment.empty() || segment == "." || segment == "..") {
                throw IOException("Unsafe ZIP entry path: " + name);
            }
            if (slash == std::string::npos) break;
            at = slash + 1;
        }
        if (!canonical_seen.insert(canonical).second) {
            throw IOException("Duplicate or ambiguous ZIP entry: " + name);
        }
        canonical_names.push_back(canonical);
        if (entry.size < 0) {
            // Unreachable: entry.size comes from the (unsigned) central directory, as in the
            // Kotlin where ZipArchiveEntry.size is only negative for an entry never read from
            // one. Kept as the same fail-closed guard.
            throw IOException("ZIP entry has an unknown declared size: " + name);
        }
        // Same fail-closed guard as the Kotlin `entry.crc < 0L`: a central-directory CRC is an
        // unsigned 32-bit value widened to Long, so it is never negative here.
        if (stored.directory) {
            if (entry.size > 0) {
                throw IOException("ZIP directory entry contains data: " + name);
            }
            continue;
        }
        state.file_order.push_back(item_index);
        if (is_in_memory_metadata(name)) {
            declared_metadata_bytes = checked_add(declared_metadata_bytes, entry.size,
                                                  "Backup metadata is too large");
            if (declared_metadata_bytes > state.metadata_limit_bytes) {
                throw IOException("Backup metadata exceeds the 256 MiB limit");
            }
        }
    }

    // The canonical-parent pass: a file cannot also be a path segment of another entry.
    for (const std::string& canonical : canonical_names) {
        std::size_t slash = canonical.rfind('/');
        while (slash != std::string::npos && slash > 0) {
            const std::string parent = canonical.substr(0, slash);
            const bool parent_is_file =
                std::any_of(state.file_order.begin(), state.file_order.end(),
                            [&](std::size_t index) {
                                return state.items[index].name == parent;
                            });
            if (parent_is_file) {
                throw IOException("Ambiguous ZIP entry path: " + canonical);
            }
            slash = parent.rfind('/');
        }
    }

    // consumeChecked over every in-memory metadata entry: the whole payload is verified at open.
    for (const std::size_t index : state.file_order) {
        const Item& item = state.items[index];
        if (!is_in_memory_metadata(item.name)) continue;
        std::string content = read_content(state, item);
        const std::int64_t declared = item.entry.size;
        const std::int64_t total = static_cast<std::int64_t>(content.size());
        if (declared >= 0 && total > declared) {
            throw IOException(item.name + " size mismatch: declared " +
                              std::to_string(declared) + " bytes, read more");
        }
        if (total > state.metadata_limit_bytes) {
            throw IOException(item.name + " exceeds the metadata size limit");
        }
        if (declared >= 0 && total != declared) {
            throw IOException(item.name + " size mismatch: declared " +
                              std::to_string(declared) + " bytes, read " +
                              std::to_string(total));
        }
        if (agora::core::zip::crc32(content) != item.entry.crc32) {
            throw IOException(item.name + " CRC mismatch");
        }
    }

    return archive;
}

NativeBackupArchive NativeBackupArchive::open_bytes(std::string bytes,
                                                    const OpenOptions& options, bool seekable) {
    if (!seekable) throw IOException(std::string(kNonSeekableMessage));
    return core_open(std::move(bytes), options);
}

NativeBackupArchive NativeBackupArchive::open_path(const std::filesystem::path& file,
                                                   const OpenOptions& options) {
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        throw IOException(file.string() + " (No such file or directory)");
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw IOException(file.string() + " (No such file or directory)");
    }
    std::string bytes((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
    try {
        NativeBackupArchive archive = core_open(std::move(bytes), options);
        if (options.owned_file) archive.owned_file_ = file;
        return archive;
    } catch (...) {
        if (options.owned_file) {
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
        }
        throw;
    }
}

const NativeBackupArchive::Item* NativeBackupArchive::find(std::string_view name) const {
    if (!state_) return nullptr;
    for (const std::size_t index : state_->file_order) {
        if (state_->items[index].name == name) return &state_->items[index];
    }
    return nullptr;
}

bool NativeBackupArchive::has(std::string_view name) const { return find(name) != nullptr; }

std::int64_t NativeBackupArchive::size(std::string_view name) const {
    const Item* item = find(name);
    return item == nullptr ? -1 : item->entry.size;
}

std::vector<std::string> NativeBackupArchive::names() const {
    std::vector<std::string> out;
    if (!state_) return out;
    out.reserve(state_->file_order.size());
    for (const std::size_t index : state_->file_order) out.push_back(state_->items[index].name);
    return out;
}

std::optional<std::string> NativeBackupArchive::bytes(std::string_view name) {
    const Item* item = find(name);
    if (item == nullptr) return std::nullopt;
    if (is_resource_entry(item->name)) {
        throw IOException("Resource entry must be copied to storage: " + item->name);
    }
    if (is_streamed_payload(item->name)) {
        throw IOException("Streamed payload must be read as a stream: " + item->name);
    }
    std::string content = read_content(*state_, *item);
    if (static_cast<std::int64_t>(content.size()) > state_->metadata_limit_bytes) {
        throw IOException(item->name + " exceeds the metadata size limit");
    }
    return content;
}

std::optional<std::string> NativeBackupArchive::prefix(std::string_view name,
                                                       long long byte_count) {
    if (byte_count < 0) throw IllegalArgumentException("Failed requirement.");
    const Item* item = find(name);
    if (item == nullptr) return std::nullopt;
    std::string content = read_content(*state_, *item);
    const auto take = static_cast<std::size_t>(std::max<long long>(byte_count, 0));
    return content.substr(0, std::min(take, content.size()));
}

std::unique_ptr<EntryStream> NativeBackupArchive::stream(std::string_view name) {
    const Item* item = find(name);
    if (item == nullptr) return nullptr;
    if (is_resource_entry(item->name)) {
        throw IOException("Resource entry must be copied to storage: " + item->name);
    }
    std::shared_ptr<State> state = state_;
    const Item entry = *item;
    auto loader = [state, entry]() { return read_content(*state, entry); };
    return std::unique_ptr<EntryStream>(
        new EntryStream(item->name, std::move(loader), item->entry.size, item->entry.crc32));
}

std::int64_t NativeBackupArchive::copy_stream_to_file(
    std::string_view input, const std::filesystem::path& target, std::int64_t declared_size,
    std::int64_t expected_crc, std::int64_t max_bytes,
    const std::function<std::int64_t()>& available_bytes, long long buffer_bytes,
    std::string_view source_name) {
    const std::string owned(input);
    return copy_stream_to_file_impl(
        [&owned] { return owned; }, target, declared_size, expected_crc, max_bytes,
        available_bytes, buffer_bytes, source_name);
}

std::optional<std::int64_t> NativeBackupArchive::copy_to(
    std::string_view name, const std::filesystem::path& target, std::int64_t max_bytes,
    const std::function<std::int64_t()>& available_bytes) {
    const Item* item = find(name);
    if (item == nullptr) return std::nullopt;
    std::shared_ptr<State> state = state_;
    const Item entry = *item;
    return copy_stream_to_file_impl(
        [state, entry]() mutable { return read_content(*state, entry); }, target,
        entry.entry.size, static_cast<std::int64_t>(entry.entry.crc32), max_bytes,
        available_bytes, kBufferBytes, std::string(name));
}

std::int64_t NativeBackupArchive::copy_stream_to_file_impl(
    const std::function<std::string()>& loader, const std::filesystem::path& target,
    std::int64_t declared_size, std::int64_t expected_crc, std::int64_t max_bytes,
    const std::function<std::int64_t()>& available_bytes, long long buffer_bytes,
    std::string_view source_name) {
    try {
        if (buffer_bytes <= 0) throw IllegalArgumentException("Failed requirement.");
        if (declared_size < -1) {
            throw IOException("Invalid declared size for " + std::string(source_name));
        }
        if (declared_size > max_bytes) {
            throw IOException(std::string(source_name) + " exceeds the import size limit");
        }
        const std::filesystem::path parent = target.parent_path();
        if (!parent.empty()) {
            std::error_code error;
            const bool is_dir = std::filesystem::is_directory(parent, error);
            if (!is_dir) {
                std::error_code create_error;
                std::filesystem::create_directories(parent, create_error);
                if (create_error || !std::filesystem::is_directory(parent, create_error)) {
                    throw IOException("Could not create import destination: " + parent.string());
                }
            }
        }
        if (declared_size >= 0) {
            ensure_available(declared_size, available_bytes(), target);
        }

        const std::string content = loader();
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw IOException("Unable to write " + target.filename().string());
        }
        std::int64_t total = 0;
        std::size_t cursor = 0;
        const std::int64_t buffer = buffer_bytes;
        while (cursor < content.size()) {
            std::int64_t read_limit = buffer;
            if (declared_size >= 0 && declared_size - total < buffer) {
                read_limit = declared_size - total + 1;
            }
            const auto remaining = static_cast<std::int64_t>(content.size() - cursor);
            const std::int64_t take = std::min(read_limit, remaining);
            if (take <= 0) break;
            total = checked_add(total, take, std::string(source_name) + " is too large");
            if (declared_size >= 0 && total > declared_size) {
                throw IOException(std::string(source_name) + " size mismatch: declared " +
                                  std::to_string(declared_size) + " bytes, read more");
            }
            if (total > max_bytes) {
                throw IOException(std::string(source_name) + " exceeds the import size limit");
            }
            ensure_available(take, available_bytes(), target);
            output.write(content.data() + cursor, static_cast<std::streamsize>(take));
            cursor += static_cast<std::size_t>(take);
        }
        output.flush();
        if (declared_size >= 0 && total != declared_size) {
            throw IOException(std::string(source_name) + " size mismatch: declared " +
                              std::to_string(declared_size) + " bytes, read " +
                              std::to_string(total));
        }
        if (expected_crc >= 0 &&
            agora::core::zip::crc32(content) != static_cast<std::uint32_t>(expected_crc)) {
            throw IOException(std::string(source_name) + " CRC mismatch");
        }
        return total;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        throw;
    }
}

std::int64_t NativeBackupArchive::preflight_import_resources(
    bool conversations_selected, bool settings_selected, int archive_version,
    const std::filesystem::path& destination_root, std::int64_t custom_font_limit_bytes,
    const std::function<std::int64_t()>& available_bytes) const {
    std::vector<const Item*> selected;
    if (conversations_selected) {
        for (const std::size_t index : state_->file_order) {
            const Item& item = state_->items[index];
            if (is_conversation_resource(item.name)) selected.push_back(&item);
        }
    }
    if (settings_selected) {
        const Item* font = nullptr;
        if (archive_version >= 4) {
            font = find(format::kCustomFontEntry);
        } else {
            for (const std::size_t index : state_->file_order) {
                const Item& item = state_->items[index];
                if (is_legacy_custom_font(item.name)) {
                    font = &item;
                    break;
                }
            }
        }
        if (font != nullptr) selected.push_back(font);
    }
    std::vector<const Item*> spooled;
    if (conversations_selected && archive_version >= 5) {
        for (const std::size_t index : state_->file_order) {
            const Item& item = state_->items[index];
            if (item.name == format::kTasksEntry ||
                starts_with(item.name, format::kConversationEntryPrefix)) {
                spooled.push_back(&item);
            }
        }
    }

    std::int64_t spool_bytes = 0;
    for (const Item* item : spooled) {
        spool_bytes = checked_add(spool_bytes, item->entry.size,
                                  "Conversation data size is too large");
    }
    std::int64_t required = spool_bytes;
    for (const Item* item : selected) {
        const std::int64_t extracted =
            (is_legacy_custom_font(item->name) && item->entry.size > custom_font_limit_bytes)
                ? 0
                : item->entry.size;
        required = checked_add(required, extracted, "Selected resource size is too large");
    }
    ensure_available(required, available_bytes(), destination_root);
    return required;
}

void NativeBackupArchive::close() {
    if (owned_file_) {
        std::error_code ignored;
        std::filesystem::remove(*owned_file_, ignored);
    }
    owned_file_.reset();
}

void NativeBackupArchive::read_to_end(EntryStream& stream) { stream.read_to_end(); }

}  // namespace agora::core::native_backup_archive
