#include "agora/core/bpe_vocabulary.hpp"

#include <cstring>
#include <optional>

#include "agora/core/unicode.hpp"

namespace agora::core {
namespace {

constexpr std::uint32_t kEmptySlot = 0;

/** FNV-1a over the byte range, sign bit folded away. */
std::uint32_t hash_of(const std::uint8_t* input, std::size_t from, std::size_t to) {
    std::uint32_t hash = 0x811C9DC5u;
    for (std::size_t at = from; at < to; ++at) {
        hash = (hash ^ input[at]) * 16777619u;
    }
    return hash & 0x7FFFFFFFu;
}

/** Kotlin's Char.isWhitespace(): JVM whitespace or space characters. */
bool is_whitespace(std::uint32_t cp) { return unicode::is_kotlin_whitespace(cp); }

bool is_blank(std::string_view line) {
    std::size_t at = 0;
    while (at < line.size()) {
        const unicode::Decoded decoded = unicode::decode_utf8(line.data() + at, line.size() - at);
        if (!is_whitespace(decoded.code_point)) return false;
        at += decoded.length;
    }
    return true;
}

/** Kotlin's String.trim(), which strips JVM-whitespace on both ends. */
std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const unicode::Decoded decoded = unicode::decode_utf8(text.data() + begin, text.size() - begin);
        if (!is_whitespace(decoded.code_point)) break;
        begin += decoded.length;
    }
    std::size_t end = text.size();
    while (end > begin) {
        const std::size_t start = unicode::previous_offset(text.data(), end);
        const unicode::Decoded decoded = unicode::decode_utf8(text.data() + start, text.size() - start);
        if (!is_whitespace(decoded.code_point)) break;
        end = start;
    }
    return text.substr(begin, end - begin);
}

int base64_value(char symbol) {
    if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
    if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
    if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
    if (symbol == '+') return 62;
    if (symbol == '/') return 63;
    return -1;
}

/** Standard base64 with padding, over `line[from, to)`. */
std::optional<std::vector<std::uint8_t>> decode_base64(std::string_view line, std::size_t from, std::size_t to) {
    std::size_t end = to;
    while (end > from && line[end - 1] == '=') end--;
    const std::size_t length = end - from;
    const std::size_t decoded_size = length * 3 / 4;
    if (decoded_size == 0) return std::nullopt;
    std::vector<std::uint8_t> out(decoded_size);
    int accumulator = 0;
    int bits_held = 0;
    std::size_t written = 0;
    for (std::size_t at = from; at < end; ++at) {
        const int value = base64_value(line[at]);
        if (value < 0) return std::nullopt;
        accumulator = (accumulator << 6) | value;
        bits_held += 6;
        if (bits_held >= 8) {
            bits_held -= 8;
            out[written++] = static_cast<std::uint8_t>((accumulator >> bits_held) & 0xFF);
        }
    }
    out.resize(written);
    return out;
}

/** Kotlin's String.toIntOrNull(): an optional sign, then digits, within Int range. */
std::optional<std::int32_t> parse_int(std::string_view text) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) return std::nullopt;
    std::size_t at = 0;
    bool negative = false;
    if (trimmed[at] == '+' || trimmed[at] == '-') {
        negative = trimmed[at] == '-';
        at++;
    }
    if (at >= trimmed.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; at < trimmed.size(); ++at) {
        const char symbol = trimmed[at];
        if (symbol < '0' || symbol > '9') return std::nullopt;
        value = value * 10 + (symbol - '0');
        if (value > 4'294'967'296LL) return std::nullopt;  // cannot fit Int afterwards
    }
    if (negative) value = -value;
    if (value < -2'147'483'648LL || value > 2'147'483'647LL) return std::nullopt;
    return static_cast<std::int32_t>(value);
}

}  // namespace

BpeVocabulary BpeVocabulary::parse_tiktoken(std::string_view text) {
    std::vector<std::vector<std::uint8_t>> all_bytes;
    std::vector<std::int32_t> all_ranks;
    std::size_t total_length = 0;

    // Mimics BufferedReader.readLine: lines end at \n, \r or \r\n, and a trailing
    // terminator does not produce an empty last line.
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t line_end = at;
        while (line_end < text.size() && text[line_end] != '\n' && text[line_end] != '\r') line_end++;
        const std::string_view line = text.substr(at, line_end - at);

        if (!is_blank(line)) {
            const std::size_t separator = line.find(' ');
            if (separator != std::string_view::npos && separator > 0) {
                const std::optional<std::int32_t> rank = parse_int(line.substr(separator + 1));
                if (rank.has_value()) {
                    std::optional<std::vector<std::uint8_t>> bytes = decode_base64(line, 0, separator);
                    if (bytes.has_value() && !bytes->empty()) {
                        total_length += bytes->size();
                        all_bytes.push_back(std::move(*bytes));
                        all_ranks.push_back(*rank);
                    }
                }
            }
        }

        if (line_end >= text.size()) break;
        at = line_end + 1;
        if (text[line_end] == '\r' && at < text.size() && text[at] == '\n') at++;
    }

    BpeVocabulary vocabulary;
    vocabulary.token_bytes_.resize(total_length);
    vocabulary.offsets_.resize(all_bytes.size() + 1);
    vocabulary.ranks_ = std::move(all_ranks);
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < all_bytes.size(); ++index) {
        vocabulary.offsets_[index] = static_cast<std::uint32_t>(cursor);
        std::memcpy(vocabulary.token_bytes_.data() + cursor, all_bytes[index].data(), all_bytes[index].size());
        cursor += all_bytes[index].size();
    }
    vocabulary.offsets_[all_bytes.size()] = static_cast<std::uint32_t>(cursor);

    // Open-addressed index from token bytes to token position, sized to stay sparse.
    std::uint32_t capacity = 1;
    while (capacity < all_bytes.size() * 2) capacity <<= 1;
    vocabulary.buckets_.assign(capacity, kEmptySlot);
    const std::uint32_t mask = capacity - 1;
    for (std::size_t index = 0; index < all_bytes.size(); ++index) {
        std::uint32_t slot = hash_of(
            vocabulary.token_bytes_.data(),
            vocabulary.offsets_[index],
            vocabulary.offsets_[index + 1]
        ) & mask;
        while (vocabulary.buckets_[slot] != kEmptySlot) slot = (slot + 1) & mask;
        vocabulary.buckets_[slot] = static_cast<std::uint32_t>(index + 1);
    }
    return vocabulary;
}

int BpeVocabulary::rank_of(const std::uint8_t* bytes, std::size_t from, std::size_t to) const {
    const std::uint32_t mask = static_cast<std::uint32_t>(buckets_.size()) - 1;
    std::uint32_t slot = hash_of(bytes, from, to) & mask;
    while (true) {
        const std::uint32_t entry = buckets_[slot];
        if (entry == kEmptySlot) return kAbsent;
        const std::size_t index = entry - 1;
        const std::uint32_t start = offsets_[index];
        const std::uint32_t end = offsets_[index + 1];
        if (end - start == to - from &&
            std::memcmp(token_bytes_.data() + start, bytes + from, to - from) == 0) {
            return ranks_[index];
        }
        slot = (slot + 1) & mask;
    }
}

}  // namespace agora::core
