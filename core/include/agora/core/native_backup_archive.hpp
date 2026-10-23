#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/zip_reader.hpp"

namespace agora::core::native_backup_archive {

/** java.io.IOException. */
class IOException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** java.lang.IllegalArgumentException. */
class IllegalArgumentException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** NativeBackupArchive.MAX_METADATA_BYTES. */
inline constexpr std::int64_t kMaxMetadataBytes = 256LL * 1024 * 1024;
inline constexpr int kBufferBytes = 32 * 1024;

/** openSeekableChannel's failure for a source without random access. */
inline constexpr std::string_view kNonSeekableMessage =
    "Backup source does not support random access; download it to local storage and select the local file";

/**
 * Platform inputs of the Android open paths: the label commons-compress names in
 * `Error reading Zip content from <label>` (the channel's class name — FileChannelImpl for both
 * file descriptors and file streams) and whether the archive owns a temporary file that close()
 * and every failed open delete, exactly like the `ownedTemporaryFile` in the Kotlin.
 */
struct OpenOptions {
    std::int64_t metadata_limit_bytes = kMaxMetadataBytes;
    std::string source_label = "FileChannelImpl";
    bool owned_file = false;
};

/**
 * CheckedEntryStream: reads one decompressed entry while checking it against its ZIP record.
 * Content is inflated on the first read (open() succeeds on a damaged streamed payload; the
 * failure surfaces when the data is actually read), size/CRC are verified when the end of the
 * data is reached, and closing early only closes — no verification happens on close.
 */
class EntryStream {
public:
    EntryStream(const EntryStream&) = delete;
    EntryStream& operator=(const EntryStream&) = delete;

    /** Returns -1 at the verified end of the stream; throws on a size/CRC mismatch. */
    long long read(char* buffer, long long count);

    /** Skipped bytes are still checked, so they are read rather than passed over. */
    long long skip(long long count);

    /** java readToEnd(): reads the rest so the stream verifies. */
    void read_to_end();

    void close();

private:
    friend class NativeBackupArchive;
    EntryStream(std::string name,
                std::function<std::string()> loader,
                std::int64_t declared_size,
                std::uint32_t expected_crc);

    void ensure_loaded();
    void verify();

    std::string name_;
    std::function<std::string()> loader_;
    std::string content_;
    bool loaded_ = false;
    bool verified_ = false;
    std::int64_t declared_size_;
    std::uint32_t expected_crc_;
    std::size_t cursor_ = 0;
    std::int64_t total_ = 0;
};

/**
 * Port of data/NativeBackupArchive.kt: an on-demand reader over a validated backup ZIP.
 *
 * Opening checks the whole central directory exactly like commons-compress does (the raw-name
 * and local-header pass, Kotlin's entry-path validation, and a full byte/CRC verification of the
 * in-memory metadata), so a preview never decompresses the streamed payloads. Commons'
 * constructor failures surface as `Error reading Zip content from <source_label>`, the label
 * being the platform channel class; Kotlin's validation failures keep their messages.
 */
class NativeBackupArchive {
public:
    /** The seekable-channel path (garbage bytes, or any source already in memory). */
    static NativeBackupArchive open_bytes(std::string bytes, const OpenOptions& options = {},
                                          bool seekable = true);

    /**
     * The local-file path: a missing file raises the JDK FileNotFoundException text
     * (`<path> (No such file or directory)`); an owned temporary file is deleted on any failure
     * and again on close().
     */
    static NativeBackupArchive open_path(const std::filesystem::path& file,
                                         const OpenOptions& options = {});

    bool has(std::string_view name) const;

    /** -1 when the archive has no such entry. */
    std::int64_t size(std::string_view name) const;

    /** null when missing; IOException for resource/streamed payloads and metadata overflow. */
    std::optional<std::string> bytes(std::string_view name);

    /** Reads up to byte_count bytes of any entry; null when missing. */
    std::optional<std::string> prefix(std::string_view name, long long byte_count);

    /** null when missing; IOException for resource payloads. */
    std::unique_ptr<EntryStream> stream(std::string_view name);

    /** Every non-directory entry name, in central-directory order. */
    std::vector<std::string> names() const;

    /**
     * copyTo: extracts one entry to disk with its size/CRC record verified. null when the entry
     * is missing. The target is deleted on any failure.
     */
    std::optional<std::int64_t> copy_to(std::string_view name,
                                        const std::filesystem::path& target,
                                        std::int64_t max_bytes,
                                        const std::function<std::int64_t()>& available_bytes);

    /** preflightImportResources: the bytes an import will need. */
    std::int64_t preflight_import_resources(
        bool conversations_selected, bool settings_selected, int archive_version,
        const std::filesystem::path& destination_root, std::int64_t custom_font_limit_bytes,
        const std::function<std::int64_t()>& available_bytes) const;

    /** Closes the archive and deletes the owned temporary file, like the Kotlin finally. */
    void close();

    /** NativeBackupArchive.copyStreamToFile — pinned directly by the stand's raw-copy ops. */
    static std::int64_t copy_stream_to_file(
        std::string_view input, const std::filesystem::path& target, std::int64_t declared_size,
        std::int64_t expected_crc, std::int64_t max_bytes,
        const std::function<std::int64_t()>& available_bytes, long long buffer_bytes,
        std::string_view source_name);

    /** java readToEnd() over an archive stream. */
    static void read_to_end(EntryStream& stream);

private:
    NativeBackupArchive() = default;

    struct State;
    struct Item;

    static NativeBackupArchive core_open(std::string bytes, const OpenOptions& options);
    static std::string read_content(State& state, const Item& item);
    static std::int64_t copy_stream_to_file_impl(
        const std::function<std::string()>& loader, const std::filesystem::path& target,
        std::int64_t declared_size, std::int64_t expected_crc, std::int64_t max_bytes,
        const std::function<std::int64_t()>& available_bytes, long long buffer_bytes,
        std::string_view source_name);

    const Item* find(std::string_view name) const;

    std::shared_ptr<State> state_;
    std::optional<std::filesystem::path> owned_file_;
};

// NativeBackupFormat.kt entry predicates, over the normalized entry name.
bool is_streamed_payload(std::string_view name);
bool is_conversation_resource(std::string_view name);
bool is_legacy_custom_font(std::string_view name);
bool is_resource_entry(std::string_view name);

}  // namespace agora::core::native_backup_archive
