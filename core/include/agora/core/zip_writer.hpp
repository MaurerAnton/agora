#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::zip {

/**
 * Writes a ZIP archive into memory (`java.util.zip.ZipOutputStream` on the platform side).
 *
 * Entries are DEFLATED (method 8) with RFC 1951 stored blocks: a valid DEFLATE stream that any
 * inflater accepts, produced without a third-party compressor (core/ carries no dependencies, the
 * same rule the reader follows). The compressed byte stream is therefore not byte-identical to
 * java.util.zip.Deflater's — the archive surface the app pins (entry names, content, CRC, sizes and
 * method) is identical, and the already-pinned Reader reads every archive this writer produces.
 *
 * Raw entries ([put_raw_entry]) preserve a source entry's compressed bytes and metadata exactly,
 * which is how an incremental baseline carries an unchanged entry forward without recompressing it.
 */
class Writer {
public:
    Writer() = default;

    /** Writes one entry, compressing the content with stored DEFLATE blocks. */
    void put_entry(std::string_view name, std::string_view content);

    /**
     * Writes one entry from an already-compressed stream (method, CRC and sizes are recorded as
     * given). The caller owns the validity of `raw`.
     */
    void put_raw_entry(std::string_view name, int method, std::uint32_t crc,
                       std::int64_t compressed_size, std::int64_t size, std::string_view raw);

    /** Finalizes the central directory and returns the archive bytes. */
    std::string finish();

private:
    struct Record {
        std::string name;
        int method = 8;
        std::uint32_t crc = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t size = 0;
        std::uint32_t offset = 0;
        std::uint16_t flags = 0;
    };

    void write_local_header(const Record& record);
    void write_central_header(const Record& record);

    std::string buffer_;
    std::vector<Record> records_;
};

}  // namespace agora::core::zip
