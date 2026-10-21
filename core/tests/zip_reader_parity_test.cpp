// Golden parity test for the ZIP reader (local headers + DEFLATE) against java.util.zip.
//
// The reader must reproduce the JDK's entry list (name, method, sizes, CRC, content) and its
// failure messages byte for byte: a foreign signature ends the archive silently, a truncated
// header is "unexpected EOF", an unsupported method is "invalid compression method", a STORED entry
// with a data descriptor is rejected, and the size/CRC checks carry the JDK's wording.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/zip_reader.hpp"
#include "check.hpp"

namespace {

namespace zip = agora::core::zip;
namespace json = agora::core::json;

using json::Kind;
using json::Member;
using json::Value;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::string decode_hex(const std::string& text) {
    std::string out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        const int high = std::isdigit(static_cast<unsigned char>(text[i]))
                             ? text[i] - '0'
                             : (std::tolower(static_cast<unsigned char>(text[i])) - 'a' + 10);
        const int low = std::isdigit(static_cast<unsigned char>(text[i + 1]))
                            ? text[i + 1] - '0'
                            : (std::tolower(static_cast<unsigned char>(text[i + 1])) - 'a' + 10);
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

/**
 * Strict UTF-8 validation (the JDK decoder runs with REPORT, so overlongs, surrogate encodings
 * and values above U+10FFFF are rejected) plus the ISO-control check the driver applies.
 */
bool valid_utf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0) != 0x80) return false;
            code_point = (code_point << 6) | (continuation & 0x3FU);
        }
        if (length == 2 && code_point < 0x80) return false;
        if (length == 3 && code_point < 0x800) return false;
        if (length == 4 && code_point < 0x10000) return false;
        if (code_point > 0x10FFFF) return false;
        if (code_point >= 0xD800 && code_point <= 0xDFFF) return false;
        i += length;
    }
    return true;
}

bool has_iso_control(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::uint32_t code_point = lead;
        std::size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code_point = lead & 0x07U;
        }
        for (std::size_t k = 1; k < length; ++k) {
            code_point = (code_point << 6) |
                         (static_cast<unsigned char>(text[i + k]) & 0x3FU);
        }
        const bool control = code_point <= 0x1F || (code_point >= 0x7F && code_point <= 0x9F);
        if (control && code_point != '\n' && code_point != '\r' && code_point != '\t') return true;
        i += length;
    }
    return false;
}

std::string hex_text(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out = "hex:";
    for (unsigned char byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

/** The driver's content rule: strict UTF-8 text unless it carries an ISO control character. */
std::string render_content(const std::string& bytes) {
    if (!valid_utf8(bytes) || has_iso_control(bytes)) return hex_text(bytes);
    return bytes;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const std::filesystem::path root_dir = path.parent_path().parent_path();
    const std::filesystem::path temp = "platform_zip.tmp";

    std::filesystem::path archive;
    if (const Value* entries = field(root, "entries"); entries != nullptr) {
        archive = root_dir / "fixtures" / (path.stem().string() + ".zip");
    } else {
        const Value* raw = field(root, "raw");
        if (raw == nullptr || raw->kind != Kind::String) scenario_fail("scenario needs entries or raw");
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        const std::string bytes = decode_hex(raw->text);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        archive = temp;
    }

    std::string trace;
    try {
        zip::Reader reader(archive.string());
        const std::vector<zip::Entry> entries = reader.entries();
        trace = "{\"entries\":[";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) trace.push_back(',');
            trace += "{\"name\":";
            append_quoted(trace, entries[i].name);
            trace += ",\"method\":" + std::to_string(entries[i].method);
            trace += ",\"size\":" + std::to_string(entries[i].size);
            trace += ",\"compressedSize\":" + std::to_string(entries[i].compressed_size);
            trace += ",\"crc\":" + std::to_string(entries[i].crc32);
            trace += ",\"content\":";
            append_quoted(trace, render_content(entries[i].content));
            trace.push_back('}');
        }
        trace += "]}";
    } catch (const zip::ZipException& error) {
        trace = "{\"error\":{\"type\":\"ZipException\",\"message\":";
        append_quoted(trace, error.what());
        trace += "}}";
    }
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(zip_reader_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_ZIP_READER_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
