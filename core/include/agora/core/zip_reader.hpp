#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace agora::core::zip {

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

/** Reads every entry of an archive in local-header order. */
class Reader {
public:
    explicit Reader(std::string path);

    /** Throws ZipException with the JDK's message on a malformed archive. */
    std::vector<Entry> entries();

private:
    std::string path_;
};

}  // namespace agora::core::zip
