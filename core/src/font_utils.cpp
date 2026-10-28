#include "agora/core/font_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace agora::core::font {

namespace {

// ---- TTF/OTF name table parsing ---------------------------------------------------

struct NameRecord {
    std::uint16_t platform_id;
    std::uint16_t encoding_id;
    std::uint16_t language_id;
    std::uint16_t name_id;
    std::uint16_t length;
    std::uint16_t offset;
};

std::uint16_t read_u16(std::istream& in) {
    std::uint8_t b1 = in.get();
    std::uint8_t b2 = in.get();
    if (!in) throw std::runtime_error("EOF reading uint16");
    return (static_cast<std::uint16_t>(b1) << 8) | b2;
}

std::uint32_t read_u32(std::istream& in) {
    std::uint16_t hi = read_u16(in);
    std::uint16_t lo = read_u16(in);
    return (static_cast<std::uint32_t>(hi) << 16) | lo;
}

std::string decode_utf16be(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 2) return "";
    try {
        // Manual UTF-16BE decode since we don't have ICU
        std::string result;
        result.reserve(bytes.size() / 2);
        for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
            std::uint16_t code = (static_cast<std::uint16_t>(bytes[i]) << 8) | bytes[i + 1];
            if (code <= 0x7F) {
                result.push_back(static_cast<char>(code));
            } else if (code <= 0x7FF) {
                result.push_back(static_cast<char>(0xC0 | (code >> 6)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code >= 0xD800 && code <= 0xDFFF) {
                // Surrogate pair - skip for simplicity (matches Kotlin fallback)
                break;
            } else {
                result.push_back(static_cast<char>(0xE0 | (code >> 12)));
                result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }
        // Trim whitespace
        auto start = result.find_first_not_of(" \t\r\n");
        auto end = result.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        return result.substr(start, end - start + 1);
    } catch (...) {
        // Fallback: filter null bytes and treat as ASCII
        std::string result;
        for (auto b : bytes) if (b != 0) result.push_back(static_cast<char>(b));
        auto start = result.find_first_not_of(" \t\r\n");
        auto end = result.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        return result.substr(start, end - start + 1);
    }
}

std::string decode_mac_roman(const std::vector<std::uint8_t>& bytes) {
    // Simplified: treat as ASCII; MacRoman is similar for common chars
    std::string result;
    for (auto b : bytes) {
        if (b < 0x80) result.push_back(static_cast<char>(b));
        else result.push_back('?');
    }
    auto start = result.find_first_not_of(" \t\r\n");
    auto end = result.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return result.substr(start, end - start + 1);
}

std::unordered_map<int, std::string> parse_ttf_name_table(std::istream& in) {
    std::unordered_map<int, std::pair<int, std::string>> results;  // nameId -> (platformId, value)

    std::uint32_t sf_version = read_u32(in);
    std::uint16_t num_tables = read_u16(in);
    in.seekg(6, std::ios::cur);  // searchRange, entrySelector, rangeShift

    std::uint32_t name_offset = 0;
    std::uint32_t name_length = 0;

    for (std::uint16_t i = 0; i < num_tables; ++i) {
        char tag_bytes[4];
        in.read(tag_bytes, 4);
        if (!in) throw std::runtime_error("EOF reading table tag");
        std::string tag(tag_bytes, 4);
        in.seekg(4, std::ios::cur);  // checkSum
        std::uint32_t offset = read_u32(in);
        std::uint32_t length = read_u32(in);
        if (tag == "name") {
            name_offset = offset;
            name_length = length;
            break;
        }
    }
    if (name_length == 0) return {};

    // Parse the 'name' table
    in.seekg(static_cast<std::streamoff>(name_offset));
    std::uint16_t format = read_u16(in);
    std::uint16_t count = read_u16(in);
    std::uint16_t string_offset = read_u16(in);
    std::uint32_t absolute_string_offset = name_offset + string_offset;

    for (std::uint16_t i = 0; i < count; ++i) {
        NameRecord rec;
        rec.platform_id = read_u16(in);
        rec.encoding_id = read_u16(in);
        rec.language_id = read_u16(in);
        rec.name_id = read_u16(in);
        rec.length = read_u16(in);
        rec.offset = read_u16(in);

        std::streampos saved_pos = in.tellg();
        std::streamoff string_pos = static_cast<std::streamoff>(absolute_string_offset + rec.offset);

        std::vector<std::uint8_t> bytes(rec.length);
        in.seekg(string_pos);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(rec.length));
        if (!in) throw std::runtime_error("EOF reading name string");

        std::string value;
        if (rec.platform_id == 3 && (rec.encoding_id == 1 || rec.encoding_id == 10)) {
            value = decode_utf16be(bytes);
        } else if (rec.platform_id == 1 && rec.encoding_id == 0) {
            value = decode_mac_roman(bytes);
        } else {
            in.seekg(saved_pos);
            continue;
        }

        if (!value.empty() && value[0] != '.') {
            auto it = results.find(static_cast<int>(rec.name_id));
            if (it == results.end() || rec.platform_id == 3) {
                results[static_cast<int>(rec.name_id)] = {static_cast<int>(rec.platform_id), value};
            }
        }

        in.seekg(saved_pos);
    }

    std::unordered_map<int, std::string> out;
    out.reserve(results.size());
    for (auto& kv : results) out[kv.first] = std::move(kv.second.second);
    return out;
}

}  // namespace

FontMetadata read_font_meta(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {file.stem().string(), std::nullopt};

    try {
        auto names = parse_ttf_name_table(in);
        std::string family;
        if (auto it = names.find(1); it != names.end()) family = it->second;
        else if (auto it = names.find(16); it != names.end()) family = it->second;
        else family = file.stem().string();

        std::optional<std::string> subfamily;
        if (auto it = names.find(2); it != names.end()) subfamily = it->second;
        else if (auto it = names.find(17); it != names.end()) subfamily = it->second;

        return {family, subfamily};
    } catch (...) {
        return {file.stem().string(), std::nullopt};
    }
}

std::string read_font_name(const std::filesystem::path& file) {
    return read_font_meta(file).family;
}

}  // namespace agora::core::font