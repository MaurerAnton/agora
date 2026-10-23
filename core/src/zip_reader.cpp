// ZIP reader (local file headers) with a from-scratch RFC 1951 DEFLATE decoder.
//
// The container rules follow java.util.zip.ZipInputStream exactly: a non-LOCSIG signature ends the
// archive silently, a truncated header is "unexpected EOF", an unsupported method is "invalid
// compression method", a STORED entry with a data descriptor is rejected, and the uncompressed size
// and the CRC are verified after the entry with the JDK's messages.
#include "agora/core/zip_reader.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace agora::core::zip {
namespace {

constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::uint32_t kDescriptorSignature = 0x08074B50;

std::uint16_t read_u16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_u32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

/** Long.toHexString: lowercase, no leading zeros. */
std::string hex_value(std::uint32_t value) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    bool leading = true;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const int digit = static_cast<int>((value >> shift) & 0xF);
        if (digit != 0 || !leading) {
            out.push_back(digits[digit]);
            leading = false;
        }
    }
    if (out.empty()) out = "0";
    return out;
}

/** java.util.zip.CRC32 (the same CRC-32/ISO-HDLC polynomial). */
std::uint32_t crc32_of(std::string_view data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> built{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0 ? (value >> 1) ^ 0xEDB88320U : value >> 1;
            }
            built[i] = value;
        }
        return built;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char byte : data) {
        crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

// ------------------------------------------------------------------ deflate (RFC 1951)

class BitReader {
public:
    explicit BitReader(const std::string& data) : data_(data) {}

    int bits(int count) {
        while (bit_count_ < count) {
            if (position_ >= data_.size()) throw ZipException("unexpected EOF");
            bit_buffer_ |= static_cast<std::uint32_t>(static_cast<unsigned char>(data_[position_]))
                           << bit_count_;
            ++position_;
            bit_count_ += 8;
        }
        const int value = static_cast<int>(bit_buffer_ & ((1U << count) - 1U));
        bit_buffer_ >>= count;
        bit_count_ -= count;
        return value;
    }

    /** Drops the partial bits of the current byte; whole buffered bytes go back to the stream. */
    void align_to_byte() {
        position_ -= static_cast<std::size_t>(bit_count_ / 8);
        bit_buffer_ = 0;
        bit_count_ = 0;
    }

    std::size_t position() const { return position_; }
    const std::string& data() const { return data_; }

    /** Moves the read position forward (used after a stored block). */
    void skip(std::size_t count) {
        if (position_ + count > data_.size()) throw ZipException("unexpected EOF");
        position_ += count;
    }

private:
    const std::string& data_;
    std::size_t position_ = 0;
    std::uint32_t bit_buffer_ = 0;
    int bit_count_ = 0;
};

struct Huffman {
    std::vector<int> count;   // codes per bit length
    std::vector<int> symbol;  // symbols ordered by code
};

/** The three Huffman trees of a DEFLATE block, with zlib's own validity messages. */
enum class TreeKind { Codes, Literal, Distance };

const char* tree_error(TreeKind kind) {
    switch (kind) {
        case TreeKind::Codes:
            return "invalid code lengths set";
        case TreeKind::Literal:
            return "invalid literal/lengths set";
        case TreeKind::Distance:
            return "invalid distances set";
    }
    return "invalid code lengths set";
}

Huffman build_huffman(const std::vector<int>& lengths, TreeKind kind = TreeKind::Literal,
                       bool validate = true) {
    Huffman huffman;
    huffman.count.assign(16, 0);
    for (int length : lengths) {
        if (length < 0 || length > 15) throw ZipException(tree_error(kind));
        ++huffman.count[static_cast<std::size_t>(length)];
    }
    huffman.count[0] = 0;
    std::vector<int> offsets(16, 0);
    for (int length = 1; length < 15; ++length) {
        offsets[static_cast<std::size_t>(length) + 1] =
            offsets[static_cast<std::size_t>(length)] + huffman.count[static_cast<std::size_t>(length)];
    }
    huffman.symbol.assign(lengths.size(), 0);
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] == 0) continue;
        huffman.symbol[static_cast<std::size_t>(
            offsets[static_cast<std::size_t>(lengths[symbol])]++)] = static_cast<int>(symbol);
    }

    // zlib's over-subscribed / incomplete check: an incomplete tree is only allowed for the
    // literal/distance trees when they hold a single code (the "one distance code" case).
    int longest = 0;
    for (int length = 15; length >= 1; --length) {
        if (huffman.count[static_cast<std::size_t>(length)] != 0) {
            longest = length;
            break;
        }
    }
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= huffman.count[static_cast<std::size_t>(length)];
        if (left < 0) throw ZipException(tree_error(kind));
    }
    if (left > 0 && validate && longest != 0 &&
        (kind == TreeKind::Codes || longest != 1)) {
        throw ZipException(tree_error(kind));
    }
    return huffman;
}

int decode_symbol(BitReader& reader, const Huffman& huffman) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= 15; ++length) {
        code |= reader.bits(1);
        const int count = huffman.count[static_cast<std::size_t>(length)];
        if (code - count < first) {
            const std::size_t slot =
                static_cast<std::size_t>(index + (code - first));
            if (slot >= huffman.symbol.size()) throw ZipException("invalid literal/length code");
            return huffman.symbol[slot];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw ZipException("invalid code -- missing end-of-block");
}

const int kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                              31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                               33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                               1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const int kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Trees {
    Huffman literal;
    Huffman distance;
};

Trees fixed_trees() {
    std::vector<int> literal_lengths(288);
    for (int symbol = 0; symbol < 144; ++symbol) literal_lengths[static_cast<std::size_t>(symbol)] = 8;
    for (int symbol = 144; symbol < 256; ++symbol) literal_lengths[static_cast<std::size_t>(symbol)] = 9;
    for (int symbol = 256; symbol < 280; ++symbol) literal_lengths[static_cast<std::size_t>(symbol)] = 7;
    for (int symbol = 280; symbol < 288; ++symbol) literal_lengths[static_cast<std::size_t>(symbol)] = 8;
    Trees trees;
    trees.literal = build_huffman(literal_lengths, TreeKind::Literal, false);
    trees.distance = build_huffman(std::vector<int>(30, 5), TreeKind::Distance, false);
    return trees;
}

Trees dynamic_trees(BitReader& reader) {
    const int literal_count = reader.bits(5) + 257;
    const int distance_count = reader.bits(5) + 1;
    const int code_count = reader.bits(4) + 4;
    if (literal_count > 286 || distance_count > 30) {
        throw ZipException("too many length or distance symbols");
    }
    static const int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
    std::vector<int> code_lengths(19, 0);
    for (int index = 0; index < code_count; ++index) {
        code_lengths[static_cast<std::size_t>(kOrder[index])] = reader.bits(3);
    }
    const Huffman code_huffman = build_huffman(code_lengths, TreeKind::Codes);

    std::vector<int> lengths(static_cast<std::size_t>(literal_count + distance_count), 0);
    int index = 0;
    while (index < literal_count + distance_count) {
        const int symbol = decode_symbol(reader, code_huffman);
        if (symbol < 16) {
            lengths[static_cast<std::size_t>(index++)] = symbol;
            continue;
        }
        int repeat = 0;
        int value = 0;
        if (symbol == 16) {
            if (index == 0) throw ZipException("invalid bit length repeat");
            value = lengths[static_cast<std::size_t>(index - 1)];
            repeat = 3 + reader.bits(2);
        } else if (symbol == 17) {
            repeat = 3 + reader.bits(3);
        } else {
            repeat = 11 + reader.bits(7);
        }
        if (index + repeat > literal_count + distance_count) {
            throw ZipException("invalid bit length repeat");
        }
        for (int step = 0; step < repeat; ++step) {
            lengths[static_cast<std::size_t>(index++)] = value;
        }
    }
    if (lengths[256] == 0) throw ZipException("invalid code -- missing end-of-block");
    Trees trees;
    trees.literal =
        build_huffman(std::vector<int>(lengths.begin(), lengths.begin() + literal_count),
                      TreeKind::Literal);
    trees.distance = build_huffman(
        std::vector<int>(lengths.begin() + literal_count, lengths.end()), TreeKind::Distance);
    return trees;
}

/**
 * Inflates one raw DEFLATE stream. `consumed` receives the number of input bytes the stream used
 * (the partially used last byte counts, exactly like zlib's bytes-read counter).
 */
std::string inflate_raw(const std::string& input, std::size_t* consumed) {
    BitReader reader(input);
    std::string out;
    const Trees fixed = fixed_trees();

    for (;;) {
        const int final_block = reader.bits(1);
        const int type = reader.bits(2);
        if (type == 0) {
            reader.align_to_byte();
            const std::size_t start = reader.position();
            if (start + 4 > reader.data().size()) throw ZipException("unexpected EOF");
            const unsigned char* header =
                reinterpret_cast<const unsigned char*>(reader.data().data()) + start;
            const int length = static_cast<int>(header[0] | (header[1] << 8));
            const int inverse = static_cast<int>(header[2] | (header[3] << 8));
            if ((length ^ 0xFFFF) != inverse) throw ZipException("invalid stored block lengths");
            if (start + 4 + static_cast<std::size_t>(length) > reader.data().size()) {
                throw ZipException("unexpected EOF");
            }
            out.append(reader.data(), start + 4, static_cast<std::size_t>(length));
            reader.skip(start + 4 + static_cast<std::size_t>(length) - reader.position());
        } else if (type == 1 || type == 2) {
            const Trees trees = type == 1 ? fixed : dynamic_trees(reader);
            for (;;) {
                const int symbol = decode_symbol(reader, trees.literal);
                if (symbol < 256) {
                    out.push_back(static_cast<char>(symbol));
                    continue;
                }
                if (symbol == 256) break;
                const int length_index = symbol - 257;
                if (length_index >= 29) throw ZipException("invalid literal/length code");
                const int length =
                    kLengthBase[length_index] + reader.bits(kLengthExtra[length_index]);
                const int distance_symbol = decode_symbol(reader, trees.distance);
                if (distance_symbol >= 30) throw ZipException("invalid distance code");
                const int distance =
                    kDistanceBase[distance_symbol] + reader.bits(kDistanceExtra[distance_symbol]);
                if (static_cast<std::size_t>(distance) > out.size()) {
                    throw ZipException("invalid distance too far back");
                }
                const std::size_t start = out.size() - static_cast<std::size_t>(distance);
                for (int step = 0; step < length; ++step) {
                    out.push_back(out[start + static_cast<std::size_t>(step)]);
                }
            }
        } else {
            throw ZipException("invalid block type");
        }
        if (final_block != 0) break;
    }
    *consumed = reader.position();
    return out;
}

}  // namespace

std::uint32_t crc32(std::string_view data) { return crc32_of(data); }

Reader::Reader(std::string path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) throw ZipException("cannot open archive");
    archive_ = std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

Reader Reader::from_bytes(std::string bytes) {
    Reader reader;
    reader.archive_ = std::move(bytes);
    return reader;
}

std::vector<Entry> Reader::entries() {
    std::vector<Entry> entries;
    Entry entry;
    while (next_entry(entry)) entries.push_back(std::move(entry));
    return entries;
}

bool Reader::next_entry(Entry& out) {
    const std::string& archive = archive_;
    std::size_t offset = offset_;
    while (offset < archive.size()) {
        if (archive.size() - offset < 4) break;  // a short tail ends the archive silently
        const unsigned char* raw = reinterpret_cast<const unsigned char*>(archive.data()) + offset;
        if (read_u32(raw) != kLocalSignature) break;
        offset += 4;
        if (archive.size() - offset < 26) throw ZipException("unexpected EOF");
        raw = reinterpret_cast<const unsigned char*>(archive.data()) + offset;
        const std::uint16_t flags = read_u16(raw + 2);
        const std::uint16_t method = read_u16(raw + 4);
        std::uint32_t crc = read_u32(raw + 10);
        std::uint32_t compressed_size = read_u32(raw + 14);
        std::uint32_t size = read_u32(raw + 18);
        const std::uint16_t name_length = read_u16(raw + 22);
        const std::uint16_t extra_length = read_u16(raw + 24);
        offset += 26;
        if (archive.size() - offset < static_cast<std::size_t>(name_length) + extra_length) {
            throw ZipException("unexpected EOF");
        }
        std::string name(archive, offset, name_length);
        offset += name_length;
        offset += extra_length;  // the extra field is skipped, like the JDK does

        const bool has_descriptor = (flags & 0x08) != 0;
        if (has_descriptor && method != 8) {
            throw ZipException("only DEFLATED entries can have EXT descriptor");
        }
        if (method != 0 && method != 8) throw ZipException("invalid compression method");

        Entry entry;
        entry.name = name;
        entry.method = method;
        std::int64_t produced = 0;
        if (method == 0) {
            if (archive.size() - offset < size) throw ZipException("unexpected EOF");
            entry.content = archive.substr(offset, size);
            offset += size;
            produced = static_cast<std::int64_t>(entry.content.size());
        } else {
            std::size_t consumed = 0;
            entry.content = inflate_raw(archive.substr(offset), &consumed);
            produced = static_cast<std::int64_t>(entry.content.size());
            if (has_descriptor) {
                offset += consumed;
                if (archive.size() - offset < 12) throw ZipException("unexpected EOF");
                const unsigned char* descriptor =
                    reinterpret_cast<const unsigned char*>(archive.data()) + offset;
                std::size_t cursor = 0;
                if (read_u32(descriptor) == kDescriptorSignature) cursor = 4;
                crc = read_u32(descriptor + cursor);
                compressed_size = read_u32(descriptor + cursor + 4);
                size = read_u32(descriptor + cursor + 8);
                offset += cursor + 12;
            } else {
                offset += compressed_size;
            }
        }

        entry.crc32 = crc;
        entry.compressed_size = static_cast<std::int64_t>(compressed_size);
        entry.size = static_cast<std::int64_t>(size);

        if (produced != entry.size) {
            throw ZipException("invalid entry size (expected " + std::to_string(entry.size) +
                                " but got " + std::to_string(produced) + " bytes)");
        }
        const std::uint32_t computed = crc32_of(entry.content);
        if (computed != entry.crc32) {
            throw ZipException("invalid entry CRC (expected 0x" + hex_value(entry.crc32) +
                                " but got 0x" + hex_value(computed) + ")");
        }
        out = std::move(entry);
        offset_ = offset;
        return true;
    }
    offset_ = offset;
    return false;
}

void Reader::parse_central_directory() {
    if (central_parsed_) return;
    const std::string& archive = archive_;
    if (archive.size() < 22) throw ZipException("archive too small for EOCD");

    // Find end of central directory record (scan from end backwards)
    constexpr std::uint32_t kEocdSignature = 0x06054B50;
    std::size_t eocd_offset = archive.size() - 22;
    std::size_t max_search = std::min<std::size_t>(archive.size(), 65536 + 22);
    bool found = false;
    for (std::size_t i = 0; i < max_search; ++i) {
        if (eocd_offset + 4 > archive.size()) break;
        const unsigned char* raw = reinterpret_cast<const unsigned char*>(archive.data()) + eocd_offset;
        if (read_u32(raw) == kEocdSignature) {
            found = true;
            break;
        }
        if (eocd_offset == 0) break;
        --eocd_offset;
    }
    if (!found) throw ZipException("end of central directory not found");

    const unsigned char* eocd = reinterpret_cast<const unsigned char*>(archive.data()) + eocd_offset;
    if (archive.size() - eocd_offset < 22) throw ZipException("unexpected EOF in EOCD");
    // disk_number = read_u16(eocd + 4);
    // disk_with_cd = read_u16(eocd + 6);
    // entries_on_disk = read_u16(eocd + 8);
    // entries_total = read_u16(eocd + 10);
    const std::uint16_t cd_entries = read_u16(eocd + 10);
    const std::uint32_t cd_size = read_u32(eocd + 12);
    const std::uint32_t cd_offset = read_u32(eocd + 16);
    // comment_length = read_u16(eocd + 20);

    if (cd_offset + cd_size > archive.size()) throw ZipException("central directory beyond archive");

    constexpr std::uint32_t kCentralSignature = 0x02014B50;
    std::size_t offset = cd_offset;
    central_entries_.reserve(cd_entries);

    for (std::uint16_t i = 0; i < cd_entries; ++i) {
        if (archive.size() - offset < 46) throw ZipException("unexpected EOF in central header");
        const unsigned char* raw = reinterpret_cast<const unsigned char*>(archive.data()) + offset;
        if (read_u32(raw) != kCentralSignature) throw ZipException("central directory signature mismatch");
        // version_made_by = read_u16(raw + 4);
        // version_needed = read_u16(raw + 6);
        // flags = read_u16(raw + 8);
        const std::uint16_t method = read_u16(raw + 10);
        // mod_time = read_u16(raw + 12);
        // mod_date = read_u16(raw + 14);
        const std::uint32_t crc = read_u32(raw + 16);
        const std::uint32_t compressed_size = read_u32(raw + 20);
        const std::uint32_t size = read_u32(raw + 24);
        const std::uint16_t name_length = read_u16(raw + 28);
        const std::uint16_t extra_length = read_u16(raw + 30);
        const std::uint16_t comment_length = read_u16(raw + 32);
        // disk_number_start = read_u16(raw + 34);
        // internal_attrs = read_u16(raw + 36);
        // external_attrs = read_u32(raw + 38);
        const std::uint32_t local_offset = read_u32(raw + 42);
        offset += 46;
        if (archive.size() - offset < static_cast<std::size_t>(name_length) + extra_length + comment_length) {
            throw ZipException("unexpected EOF in central header fields");
        }
        std::string name(archive, offset, name_length);
        const unsigned char* extra = reinterpret_cast<const unsigned char*>(archive.data()) +
                                     offset + name_length;
        const std::size_t extra_length_all = extra_length;

        std::uint64_t size_value = size;
        std::uint64_t compressed_value = compressed_size;
        std::uint64_t offset_value = local_offset;
        // ZIP64 extended information (id 0x0001): only the fields still carrying their
        // 0xFFFFFFFF placeholder are present, in uncompressed/compressed/offset order.
        if (size == 0xFFFFFFFFu || compressed_size == 0xFFFFFFFFu || local_offset == 0xFFFFFFFFu) {
            std::size_t at = 0;
            while (at + 4 <= extra_length_all) {
                const std::uint16_t id = static_cast<std::uint16_t>(extra[at] | (extra[at + 1] << 8));
                const std::uint16_t len = static_cast<std::uint16_t>(extra[at + 2] | (extra[at + 3] << 8));
                at += 4;
                if (at + len > extra_length_all) break;
                if (id == 0x0001) {
                    std::size_t data = at;
                    const std::size_t data_end = at + len;
                    const auto take = [&](std::uint64_t& target) {
                        if (data + 8 <= data_end) {
                            std::uint64_t value = 0;
                            for (int i = 7; i >= 0; --i) value = (value << 8) | extra[data + i];
                            target = value;
                            data += 8;
                        }
                    };
                    if (size == 0xFFFFFFFFu) take(size_value);
                    if (compressed_size == 0xFFFFFFFFu) take(compressed_value);
                    if (local_offset == 0xFFFFFFFFu) take(offset_value);
                    break;
                }
                at += len;
            }
        }
        offset += name_length + extra_length + comment_length;

        CentralEntry ce;
        ce.name = std::move(name);
        ce.method = static_cast<int>(method);
        ce.size = static_cast<std::int64_t>(size_value);
        ce.compressed_size = static_cast<std::int64_t>(compressed_value);
        ce.crc32 = crc;
        ce.local_offset = static_cast<std::uint64_t>(offset_value);
        central_entries_.push_back(std::move(ce));
    }
    central_parsed_ = true;
}

std::vector<CentralEntry> Reader::central_entries() {
    parse_central_directory();
    return central_entries_;
}

std::string Reader::read_at(std::uint64_t local_offset, std::int64_t expected_size,
                            std::uint32_t expected_crc) {
    if (local_offset > archive_.size()) throw ZipException("local offset beyond archive");
    std::size_t offset = static_cast<std::size_t>(local_offset);
    const std::string& archive = archive_;
    if (archive.size() - offset < 30) throw ZipException("unexpected EOF at local header");
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(archive.data()) + offset;
    if (read_u32(raw) != kLocalSignature) throw ZipException("local signature mismatch at offset");
    const std::uint16_t flags = read_u16(raw + 6);
    const std::uint16_t method = read_u16(raw + 8);
    // local crc/size/compressed_size may be 0 if data descriptor is used; we trust central values
    const std::uint16_t name_length = read_u16(raw + 26);
    const std::uint16_t extra_length = read_u16(raw + 28);
    offset += 30 + name_length + extra_length;

    const bool has_descriptor = (flags & 0x08) != 0;
    if (has_descriptor && method != 8) {
        throw ZipException("only DEFLATED entries can have EXT descriptor");
    }
    if (method != 0 && method != 8) throw ZipException("invalid compression method");

    std::string content;
    if (method == 0) {
        if (archive.size() - offset < static_cast<std::size_t>(expected_size)) {
            throw ZipException("unexpected EOF in stored entry");
        }
        content = archive.substr(offset, static_cast<std::size_t>(expected_size));
    } else {
        std::size_t consumed = 0;
        content = inflate_raw(archive.substr(offset), &consumed);
    }

    const std::uint32_t computed = crc32_of(content);
    if (computed != expected_crc) {
        throw ZipException("invalid entry CRC (expected 0x" + hex_value(expected_crc) +
                            " but got 0x" + hex_value(computed) + ")");
    }
    if (static_cast<std::int64_t>(content.size()) != expected_size) {
        throw ZipException("invalid entry size (expected " + std::to_string(expected_size) +
                            " but got " + std::to_string(content.size()) + " bytes)");
    }
    return content;
}

std::string Reader::read_at_unchecked(std::uint64_t local_offset, std::int64_t expected_size) {
    if (local_offset > archive_.size()) throw ZipException("local offset beyond archive");
    std::size_t offset = static_cast<std::size_t>(local_offset);
    const std::string& archive = archive_;
    if (archive.size() - offset < 30) throw ZipException("unexpected EOF at local header");
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(archive.data()) + offset;
    if (read_u32(raw) != kLocalSignature) throw ZipException("local signature mismatch at offset");
    const std::uint16_t flags = read_u16(raw + 6);
    const std::uint16_t method = read_u16(raw + 8);
    const std::uint16_t name_length = read_u16(raw + 26);
    const std::uint16_t extra_length = read_u16(raw + 28);
    offset += 30 + name_length + extra_length;

    const bool has_descriptor = (flags & 0x08) != 0;
    if (has_descriptor && method != 8) {
        throw ZipException("only DEFLATED entries can have EXT descriptor");
    }
    if (method != 0 && method != 8) throw ZipException("invalid compression method");

    if (method == 0) {
        if (archive.size() - offset < static_cast<std::size_t>(expected_size)) {
            throw ZipException("unexpected EOF in stored entry");
        }
        return archive.substr(offset, static_cast<std::size_t>(expected_size));
    }
    std::size_t consumed = 0;
    return inflate_raw(archive.substr(offset), &consumed);
}

std::string Reader::raw_at(std::uint64_t local_offset, std::int64_t compressed_size) {
    if (local_offset > archive_.size()) throw ZipException("local offset beyond archive");
    std::size_t offset = static_cast<std::size_t>(local_offset);
    const std::string& archive = archive_;
    if (archive.size() - offset < 30) throw ZipException("unexpected EOF at local header");
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(archive.data()) + offset;
    if (read_u32(raw) != kLocalSignature) throw ZipException("local signature mismatch at offset");
    const std::uint16_t name_length = read_u16(raw + 26);
    const std::uint16_t extra_length = read_u16(raw + 28);
    offset += 30 + name_length + extra_length;
    if (compressed_size < 0 ||
        archive.size() - offset < static_cast<std::size_t>(compressed_size)) {
        throw ZipException("unexpected EOF in raw entry");
    }
    return archive.substr(offset, static_cast<std::size_t>(compressed_size));
}

std::string Reader::read_named(const std::string& name) {
    parse_central_directory();
    for (const auto& ce : central_entries_) {
        if (ce.name == name) {
            return read_at(ce.local_offset, ce.size, ce.crc32);
        }
    }
    throw ZipException("entry not found: " + name);
}

}  // namespace agora::core::zip
