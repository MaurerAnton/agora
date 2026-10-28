// Unit tests for the FontUtils port: TTF/OTF name table parsing.

#include "agora/core/font_utils.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"

namespace font = agora::core::font;

namespace {

// Creates a minimal valid TTF file with a name table containing nameID 1 and 2.
std::vector<std::uint8_t> make_minimal_ttf(const std::string& family, const std::string& subfamily) {
    std::vector<std::uint8_t> data;

    // Helper lambdas
    auto push_u16 = [&](std::uint16_t v) {
        data.push_back(static_cast<std::uint8_t>(v >> 8));
        data.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };
    auto push_u32 = [&](std::uint32_t v) {
        data.push_back(static_cast<std::uint8_t>(v >> 24));
        data.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        data.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        data.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };
    auto push_string = [&](const std::string& s) {
        for (char c : s) data.push_back(static_cast<std::uint8_t>(c));
    };

    // TTF header
    push_u32(0x00010000);  // sfVersion
    push_u16(1);           // numTables (just 'name' table)
    push_u16(0); push_u16(0); push_u16(0);  // searchRange, entrySelector, rangeShift

    // Table directory entry for 'name'
    push_string("name");
    push_u32(0);           // checkSum
    const std::uint32_t name_offset = 12 + 16;  // header + 1 table directory
    push_u32(name_offset);
    // name_length will be filled later
    const std::size_t name_length_pos = data.size();
    push_u32(0);           // placeholder for length

    // ===== 'name' table starts here =====
    std::size_t name_table_start = data.size();
    push_u16(0);           // format = 0
    push_u16(2);           // count = 2 records
    push_u16(6 + 2 * 12);  // stringOffset = 6 + 2*12 = 30

    // Record 1: nameID 1 (family), Windows platform (3), encoding 1, language 0x0409
    push_u16(3);   // platformID
    push_u16(1);   // encodingID
    push_u16(0x0409); // languageID
    push_u16(1);   // nameID
    push_u16(static_cast<std::uint16_t>(family.size() * 2)); // length (UTF-16BE)
    push_u16(0);   // offset = 0

    // Record 2: nameID 2 (subfamily), Windows platform (3), encoding 1, language 0x0409
    push_u16(3);
    push_u16(1);
    push_u16(0x0409);
    push_u16(2);
    push_u16(static_cast<std::uint16_t>(subfamily.size() * 2));
    push_u16(static_cast<std::uint16_t>(family.size() * 2)); // offset after family

    // String data (UTF-16BE)
    for (char c : family) {
        data.push_back(0);  // high byte
        data.push_back(static_cast<std::uint8_t>(c)); // low byte
    }
    for (char c : subfamily) {
        data.push_back(0);
        data.push_back(static_cast<std::uint8_t>(c));
    }

    std::size_t name_table_end = data.size();
    std::uint32_t name_len = static_cast<std::uint32_t>(name_table_end - name_table_start);

    // Fix the name table length in the table directory
    data[name_length_pos] = static_cast<std::uint8_t>(name_len >> 24);
    data[name_length_pos + 1] = static_cast<std::uint8_t>((name_len >> 16) & 0xFF);
    data[name_length_pos + 2] = static_cast<std::uint8_t>((name_len >> 8) & 0xFF);
    data[name_length_pos + 3] = static_cast<std::uint8_t>(name_len & 0xFF);

    return data;
}

}  // namespace

TEST(font_utils_reads_family_and_subfamily) {
    std::filesystem::path temp = std::filesystem::temp_directory_path() / "test_font.ttf";
    std::error_code ec;

    auto ttf = make_minimal_ttf("TestFont", "Bold");
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ttf.data()), ttf.size());
    }

    auto meta = font::read_font_meta(temp);
    CHECK_EQ(std::string("TestFont"), meta.family);
    CHECK(meta.subfamily.has_value());
    CHECK_EQ(std::string("Bold"), *meta.subfamily);

    std::filesystem::remove(temp, ec);
}

TEST(font_utils_reads_font_name) {
    std::filesystem::path temp = std::filesystem::temp_directory_path() / "test_font2.ttf";
    std::error_code ec;

    auto ttf = make_minimal_ttf("AnotherFont", "Regular");
    {
        std::ofstream out(temp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(ttf.data()), ttf.size());
    }

    std::string name = font::read_font_name(temp);
    CHECK_EQ(std::string("AnotherFont"), name);

    std::filesystem::remove(temp, ec);
}

TEST(font_utils_fallback_to_filename) {
    std::filesystem::path temp = std::filesystem::temp_directory_path() / "NoNameFont.ttf";
    std::error_code ec;

    // Invalid TTF (no name table)
    {
        std::ofstream out(temp, std::ios::binary);
        out.write("not a ttf", 9);
    }

    auto meta = font::read_font_meta(temp);
    CHECK_EQ(std::string("NoNameFont"), meta.family);
    CHECK(!meta.subfamily.has_value());

    std::filesystem::remove(temp, ec);
}
