#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::zip {

/** java.util.zip.CRC32 (CRC-32/ISO-HDLC) over the given bytes. */
std::uint32_t crc32(std::string_view data);

/**
 * A streaming ZIP reader for the archive surface the app uses to read a vendor export
 * (java.util.zip.ZipInputStream): local file headers, STORED and DEFLATED entries, data
 * descriptors and the JDK's failure messages.
 *
 * The DEFLATE decoder is a from-scratch RFC 1951 implementation (no third-party dependency is
 * allowed in core/). The container rules and the exception texts are pinned against the JDK by the
 * `zip-reader` golden stand.
 */
struct Entry {
    std::string name;
    int method = 0;                 // 0 = STORED, 8 = DEFLATED
    std::int64_t size = 0;          // uncompressed size
    std::int64_t compressed_size = 0;
    std::uint32_t crc32 = 0;
    std::string content;
};

/** java.util.zip.ZipException. */
class ZipException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Central directory entry — metadata without content. Used for random access without inflating
 * the entire archive.
 */
struct CentralEntry {
    std::string name;
    int method = 0;
    std::int64_t size = 0;
    std::int64_t compressed_size = 0;
    std::uint32_t crc32 = 0;
    std::uint64_t local_offset = 0;  // offset of the local file header
};

/** Reads the entries of an archive in local-header order. */
class Reader {
public:
    explicit Reader(std::string path);

    /** Reads an archive that is already in memory (the importers receive bytes, not a file). */
    static Reader from_bytes(std::string bytes);

    /** Throws ZipException with the JDK's message on a malformed archive. */
    std::vector<Entry> entries();

    /**
     * Streaming variant: reads the next entry and returns false at the end of the archive.
     * The importer stops at the first entry it can use, so the reader must not read ahead.
     */
    bool next_entry(Entry& out);

    /**
     * Parses the central directory and returns every entry's metadata without inflating
     * content. This is how java.util.zip.ZipFile achieves random access.
     */
    std::vector<CentralEntry> central_entries();

    /**
     * Reads and inflates the entry at `local_offset`, validating its CRC against the
     * recorded value. Throws ZipException on CRC mismatch or decode failure.
     */
    std::string read_at(std::uint64_t local_offset, std::int64_t expected_size,
                        std::uint32_t expected_crc);

    /**
     * Reads and decompresses the entry without checking the CRC or the size — the semantics of
     * java.util.zip.ZipFile.getInputStream, which only fails when the compressed stream itself
     * is damaged.
     */
    std::string read_at_unchecked(std::uint64_t local_offset, std::int64_t expected_size);

    /**
     * Returns the raw compressed bytes of the entry exactly as stored (no inflation, no
     * validation), for byte-preserving archive copies.
     */
    std::string raw_at(std::uint64_t local_offset, std::int64_t compressed_size);

    /**
     * Convenience: find entry by name in the central directory and read it.
     * Throws ZipException if not found or on CRC mismatch.
     */
    std::string read_named(const std::string& name);

private:
    Reader() = default;

    std::string archive_;
    std::size_t offset_ = 0;
    bool central_parsed_ = false;
    std::vector<CentralEntry> central_entries_;

    void parse_central_directory();
};

}  // namespace agora::core::zip