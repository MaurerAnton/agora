// ZIP writer with a from-scratch RFC 1951 encoder.
//
// Entries are DEFLATED with stored (uncompressed) DEFLATE blocks: the encoder emits BFINAL/BTYPE
// headers, LEN/NLEN and the literal bytes. That stream is valid RFC 1951 and is accepted by every
// inflater (including the core reader and java.util.zip), so the archive is real — it is simply
// not entropy-coded, because core/ may not depend on a compressor and the app's pinned surface is
// the entry list, not the exact compressed byte counts. Raw entries keep a source entry's
// compressed stream untouched.
#include "agora/core/zip_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

#include "agora/core/zip_reader.hpp"

namespace agora::core::zip {
namespace {

constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::uint32_t kCentralSignature = 0x02014B50;
constexpr std::uint32_t kEocdSignature = 0x06054B50;
constexpr std::uint16_t kVersion = 20;
constexpr std::uint16_t kUtf8Flag = 0x0800;

void write_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void write_u32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

bool needs_utf8_flag(std::string_view name) {
    for (unsigned char byte : name) {
        if (byte >= 0x80) return true;
    }
    return false;
}

/** RFC 1951 stored blocks; the last block carries BFINAL. */
std::string deflate_stored(std::string_view content) {
    std::string out;
    std::size_t position = 0;
    std::size_t remaining = content.size();
    do {
        const std::size_t chunk = std::min<std::size_t>(remaining, 65535);
        const bool final_block = remaining == chunk;
        out.push_back(final_block ? '\x01' : '\x00');
        write_u16(out, static_cast<std::uint16_t>(chunk));
        write_u16(out, static_cast<std::uint16_t>(~static_cast<std::uint16_t>(chunk)));
        out.append(content.data() + position, chunk);
        position += chunk;
        remaining -= chunk;
    } while (remaining > 0);
    return out;
}

}  // namespace

void Writer::put_entry(std::string_view name, std::string_view content) {
    const std::string compressed = deflate_stored(content);
    Record record;
    record.name = std::string(name);
    record.method = 8;
    record.crc = crc32(content);
    record.compressed_size = static_cast<std::uint32_t>(compressed.size());
    record.size = static_cast<std::uint32_t>(content.size());
    record.offset = static_cast<std::uint32_t>(buffer_.size());
    record.flags = needs_utf8_flag(name) ? kUtf8Flag : 0;

    write_local_header(record);
    buffer_ += compressed;
    records_.push_back(std::move(record));
}

void Writer::put_raw_entry(std::string_view name, int method, std::uint32_t crc,
                           std::int64_t compressed_size, std::int64_t size,
                           std::string_view raw) {
    Record record;
    record.name = std::string(name);
    record.method = method;
    record.crc = crc;
    record.compressed_size = static_cast<std::uint32_t>(compressed_size);
    record.size = static_cast<std::uint32_t>(size);
    record.offset = static_cast<std::uint32_t>(buffer_.size());
    record.flags = needs_utf8_flag(name) ? kUtf8Flag : 0;

    write_local_header(record);
    buffer_.append(raw.data(), raw.size());
    records_.push_back(std::move(record));
}

void Writer::write_local_header(const Record& record) {
    write_u32(buffer_, kLocalSignature);
    write_u16(buffer_, kVersion);
    write_u16(buffer_, record.flags);
    write_u16(buffer_, static_cast<std::uint16_t>(record.method));
    write_u16(buffer_, 0);  // mod time
    write_u16(buffer_, 0);  // mod date
    write_u32(buffer_, record.crc);
    write_u32(buffer_, record.compressed_size);
    write_u32(buffer_, record.size);
    write_u16(buffer_, static_cast<std::uint16_t>(record.name.size()));
    write_u16(buffer_, 0);  // extra length
    buffer_ += record.name;
}

void Writer::write_central_header(const Record& record) {
    write_u32(buffer_, kCentralSignature);
    write_u16(buffer_, kVersion);  // version made by
    write_u16(buffer_, kVersion);  // version needed
    write_u16(buffer_, record.flags);
    write_u16(buffer_, static_cast<std::uint16_t>(record.method));
    write_u16(buffer_, 0);  // mod time
    write_u16(buffer_, 0);  // mod date
    write_u32(buffer_, record.crc);
    write_u32(buffer_, record.compressed_size);
    write_u32(buffer_, record.size);
    write_u16(buffer_, static_cast<std::uint16_t>(record.name.size()));
    write_u16(buffer_, 0);  // extra length
    write_u16(buffer_, 0);  // comment length
    write_u16(buffer_, 0);  // disk number start
    write_u16(buffer_, 0);  // internal attributes
    write_u32(buffer_, 0);  // external attributes
    write_u32(buffer_, record.offset);
    buffer_ += record.name;
}

std::string Writer::finish() {
    const std::uint32_t central_offset = static_cast<std::uint32_t>(buffer_.size());
    for (const Record& record : records_) write_central_header(record);
    const std::uint32_t central_size = static_cast<std::uint32_t>(buffer_.size()) - central_offset;

    write_u32(buffer_, kEocdSignature);
    write_u16(buffer_, 0);  // disk number
    write_u16(buffer_, 0);  // disk with central directory
    write_u16(buffer_, static_cast<std::uint16_t>(records_.size()));
    write_u16(buffer_, static_cast<std::uint16_t>(records_.size()));
    write_u32(buffer_, central_size);
    write_u32(buffer_, central_offset);
    write_u16(buffer_, 0);  // comment length
    return std::move(buffer_);
}

}  // namespace agora::core::zip
