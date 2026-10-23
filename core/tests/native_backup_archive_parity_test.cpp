// Golden parity test for the C++ NativeBackupArchive port.
//
// Replays every scenario under core/tools/golden/native-backup-archive/scenarios and requires
// the produced trace to equal the frozen Kotlin trace byte for byte. The trace format mirrors
// the reference driver (core/tools/golden/native-backup-archive/NativeBackupArchiveTraceDump.kt),
// including the op groups, the `message ?: simpleName` error rule, the text/hex content rule,
// the `<dir>` path mask, and the driver's own stream state machine ("Archive is not open",
// "Stream not open: <key>").
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/native_backup_archive.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace archive = agora::core::native_backup_archive;

using archive::EntryStream;
using archive::NativeBackupArchive;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

/** Strict UTF-8 validation matching CharsetDecoder with REPORT (overlong/surrogate rejected). */
bool valid_utf8(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) { ++i; continue; }
        std::size_t length = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            if (lead < 0xC2) return false;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            if (lead > 0xF4) return false;
        } else {
            return false;
        }
        if (i + length > bytes.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80) return false;
        }
        if (length == 3) {
            const auto second = static_cast<unsigned char>(bytes[i + 1]);
            if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0)) return false;
        }
        if (length == 4) {
            const auto second = static_cast<unsigned char>(bytes[i + 1]);
            if ((lead == 0xF0 && second < 0x90) || (lead > 0xF4)) return false;
            if (lead == 0xF4 && second >= 0x90) return false;
        }
        i += length;
    }
    return true;
}

/** zip-reader's shared content rule: strict UTF-8 without ISO control characters → text. */
std::string render(std::string_view bytes) {
    const bool control_free =
        std::none_of(bytes.begin(), bytes.end(), [](char raw) {
            const auto c = static_cast<unsigned char>(raw);
            return c < 0x20 || (c >= 0x7F && c <= 0x9F);
        });
    if (valid_utf8(bytes) && control_free) return std::string(bytes);
    static const char* kHex = "0123456789abcdef";
    std::string out = "hex:";
    for (char raw : bytes) {
        const auto c = static_cast<unsigned char>(raw);
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) return &entry.value;
    }
    return nullptr;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == json::Kind::Literal && value->text == "null");
}

std::string string_or(const json::Value& object, std::string_view name,
                      const std::string& fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name), "expected a string");
    return value->text;
}

bool bool_or(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected a boolean");
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    scenario_fail(std::string(name), "expected a boolean");
}

std::optional<std::int64_t> optional_long(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind != json::Kind::Literal) {
        scenario_fail(std::string(name), "expected a number");
    }
    try {
        std::size_t used = 0;
        const long long parsed = std::stoll(value->text, &used);
        if (used != value->text.size()) scenario_fail(std::string(name), "expected a number");
        return parsed;
    } catch (const std::exception&) {
        scenario_fail(std::string(name), "expected a number");
    }
}

std::vector<std::string> string_list(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != json::Kind::Array) scenario_fail(std::string(name), "expected an array");
    std::vector<std::string> out;
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::String) scenario_fail(std::string(name), "expected strings");
        out.push_back(element.text);
    }
    return out;
}

struct Step {
    std::string op;
    std::string mode;
    std::string name;
    std::string key;
    long long count = 0;
    std::string target;
    std::string dest;
    bool conversations = false;
    bool settings = false;
    int version = 0;
    std::optional<std::int64_t> metadata_limit;
    std::optional<std::int64_t> max_bytes;
    std::optional<std::int64_t> available;
    std::optional<std::int64_t> font_limit;
    std::string channel_fixture;
    std::optional<std::int64_t> declared_size;
    std::optional<std::int64_t> expected_crc;
    std::optional<long long> buffer_bytes;
    std::string source_name;
    std::string input;
};

Step parse_step(const json::Value& value) {
    Step step;
    step.op = string_or(value, "op", "");
    step.mode = string_or(value, "mode", "");
    step.name = string_or(value, "name", "");
    step.key = string_or(value, "key", "");
    step.count = optional_long(value, "count").value_or(0);
    step.target = string_or(value, "target", "");
    step.dest = string_or(value, "dest", "");
    step.conversations = bool_or(value, "conversations", false);
    step.settings = bool_or(value, "settings", false);
    step.version = static_cast<int>(optional_long(value, "version").value_or(0));
    step.metadata_limit = optional_long(value, "metadataLimit");
    step.max_bytes = optional_long(value, "maxBytes");
    step.available = optional_long(value, "available");
    step.font_limit = optional_long(value, "fontLimit");
    step.channel_fixture = string_or(value, "channelFixture", "");
    step.declared_size = optional_long(value, "declaredSize");
    step.expected_crc = optional_long(value, "expectedCrc");
    step.buffer_bytes = optional_long(value, "bufferBytes");
    step.source_name = string_or(value, "sourceName", "");
    step.input = string_or(value, "input", "");
    return step;
}

class Trace {
public:
    Trace(std::filesystem::path work_dir) : work_dir_(std::move(work_dir)) {}

    void line(std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& value : fields) {
            if (!first) out_ << '|';
            first = false;
            out_ << esc(mask(value));
        }
        out_ << '\n';
    }

    void raw_line(std::string_view text) { out_ << text << '\n'; }

    std::string str() const { return out_.str(); }

private:
    std::string mask(std::string_view value) const {
        std::string text(value);
        const std::string root = work_dir_.string();
        std::size_t at = 0;
        while ((at = text.find(root, at)) != std::string::npos) {
            text.replace(at, root.size(), "<dir>");
            at += 5;
        }
        return text;
    }

    std::filesystem::path work_dir_;
    std::ostringstream out_;
};

std::string run_scenario(const std::filesystem::path& scenario_path,
                         const std::filesystem::path& temp_root,
                         const std::filesystem::path& fixtures_dir) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;
    const std::string name = string_or(scenario, "name", stem);
    if (name != stem) scenario_fail(stem, "scenario name must match the file stem");
    const std::string archive_name = string_or(scenario, "archive", "");
    const std::vector<std::string> setup_files = string_list(scenario, "setupFiles");

    const std::filesystem::path work_dir = temp_root / ("scenario_" + name);
    std::filesystem::remove_all(work_dir);
    std::filesystem::create_directories(work_dir);

    const std::filesystem::path fixture = fixtures_dir / archive_name;
    const std::filesystem::path owned_copy = work_dir / fixture.filename();
    if (std::filesystem::is_regular_file(fixture)) {
        std::filesystem::copy_file(fixture, owned_copy,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    for (const std::string& rel : setup_files) {
        const std::filesystem::path file = work_dir / rel;
        std::filesystem::create_directories(file.parent_path());
        write_file(file, "");
    }

    Trace trace(work_dir);
    std::optional<NativeBackupArchive> open_archive;
    std::map<std::string, std::unique_ptr<EntryStream>> streams;

    const auto require_archive = [&]() -> NativeBackupArchive& {
        if (!open_archive) throw std::runtime_error("Archive is not open");
        return *open_archive;
    };
    const auto require_stream = [&](const std::string& key) -> EntryStream& {
        auto found = streams.find(key);
        if (found == streams.end()) throw std::runtime_error("Stream not open: " + key);
        return *found->second;
    };
    const auto available_of = [](std::optional<std::int64_t> value) {
        return [value]() -> std::int64_t {
            return value.value_or(std::numeric_limits<std::int64_t>::max());
        };
    };

    const json::Value* steps = field(scenario, "ops");
    if (steps == nullptr || steps->kind != json::Kind::Array) {
        scenario_fail(stem, "missing ops");
    }

    std::size_t index = 0;
    for (const json::Value& value : steps->elements) {
        const Step step = parse_step(value);
        trace.raw_line("op|" + std::to_string(index) + "|" + step.op);
        ++index;
        try {
            if (step.op == "open") {
                archive::OpenOptions options;
                options.metadata_limit_bytes =
                    step.metadata_limit.value_or(archive::kMaxMetadataBytes);
                NativeBackupArchive* opened = nullptr;
                if (step.mode == "file") {
                    options.source_label = "FileChannelImpl";
                    options.owned_file = true;
                    open_archive.emplace(
                        NativeBackupArchive::open_path(owned_copy, options));
                    opened = &*open_archive;
                } else if (step.mode == "uri") {
                    // open(context, uri) has no metadataLimit parameter: the default applies.
                    options.metadata_limit_bytes = archive::kMaxMetadataBytes;
                    options.source_label = "FileChannelImpl";
                    options.owned_file = false;
                    open_archive.emplace(
                        NativeBackupArchive::open_path(owned_copy, options));
                    opened = &*open_archive;
                } else if (step.mode == "uri_null") {
                    trace.line({"open", "null"});
                    continue;
                } else if (step.mode == "channel") {
                    const std::filesystem::path source = step.channel_fixture.empty()
                                                             ? fixture
                                                             : fixtures_dir / step.channel_fixture;
                    options.source_label = "BytesChannel";
                    options.owned_file = false;
                    open_archive.emplace(NativeBackupArchive::open_bytes(
                        read_file(source), options, /*seekable=*/true));
                    opened = &*open_archive;
                } else if (step.mode == "nonseek") {
                    options.source_label = "NonSeekableChannel";
                    open_archive.emplace(NativeBackupArchive::open_bytes(
                        "", options, /*seekable=*/false));
                    opened = &*open_archive;
                } else {
                    throw std::runtime_error("unknown open mode: " + step.mode);
                }
                (void)opened;
                trace.line({"open", "ok"});
                continue;
            }
            if (step.op == "temp_exists") {
                trace.line({"exists", std::filesystem::exists(owned_copy) ? "true" : "false"});
                continue;
            }
            if (step.op == "copy_stream_raw") {
                const std::filesystem::path target = work_dir / step.target;
                try {
                    const std::string source_name =
                        step.source_name.empty() ? target.filename().string() : step.source_name;
                    const auto copied = NativeBackupArchive::copy_stream_to_file(
                        step.input, target, step.declared_size.value_or(-1),
                        step.expected_crc.value_or(-1),
                        step.max_bytes.value_or(std::numeric_limits<std::int64_t>::max()),
                        available_of(step.available),
                        step.buffer_bytes.value_or(32 * 1024), source_name);
                    trace.line({"copy", std::to_string(copied), render(read_file(target))});
                } catch (const std::exception& failure) {
                    trace.line({"error", failure.what()});
                    trace.line({"exists", "false"});
                }
                continue;
            }
            if (step.op == "copy_to") {
                const std::filesystem::path target = work_dir / step.target;
                try {
                    const auto copied = require_archive().copy_to(
                        step.name, target,
                        step.max_bytes.value_or(std::numeric_limits<std::int64_t>::max()),
                        available_of(step.available));
                    if (!copied.has_value()) {
                        trace.line({"copy", "null"});
                        trace.line({"exists", "false"});
                    } else {
                        trace.line({"copy", std::to_string(*copied), render(read_file(target))});
                    }
                } catch (const std::exception& failure) {
                    trace.line({"error", failure.what()});
                    trace.line({"exists", "false"});
                }
                continue;
            }

            // Every remaining op requires an open archive and one uniform error shape.
            NativeBackupArchive& backup = require_archive();
            if (step.op == "names") {
                const auto names = backup.names();
                trace.line({"names", std::to_string(names.size())});
                for (const std::string& entry : names) trace.line({"entry", entry});
            } else if (step.op == "size") {
                trace.line({"size", std::to_string(backup.size(step.name))});
            } else if (step.op == "has") {
                trace.line({"has", backup.has(step.name) ? "true" : "false"});
            } else if (step.op == "bytes") {
                const auto content = backup.bytes(step.name);
                if (!content.has_value()) {
                    trace.line({"bytes", "null"});
                } else {
                    trace.line({"bytes", std::to_string(content->size()), render(*content)});
                }
            } else if (step.op == "prefix") {
                const auto content = backup.prefix(step.name, step.count);
                if (!content.has_value()) {
                    trace.line({"prefix", "null"});
                } else {
                    trace.line({"prefix", std::to_string(content->size()), render(*content)});
                }
            } else if (step.op == "stream_open") {
                auto stream = backup.stream(step.name);
                if (!stream) {
                    trace.line({"stream", step.key, "missing"});
                } else {
                    streams[step.key] = std::move(stream);
                    trace.line({"stream", step.key, "opened"});
                }
            } else if (step.op == "stream_read") {
                EntryStream& stream = require_stream(step.key);
                std::string buffer(static_cast<std::size_t>(std::max<long long>(step.count, 0)),
                                   '\0');
                const long long read =
                    stream.read(buffer.data(), static_cast<long long>(buffer.size()));
                if (read < 0) {
                    trace.line({"read", "-1"});
                } else {
                    trace.line({"read", std::to_string(read),
                                render(std::string_view(buffer.data(),
                                                        static_cast<std::size_t>(read)))});
                }
            } else if (step.op == "stream_skip") {
                EntryStream& stream = require_stream(step.key);
                trace.line({"skip", std::to_string(stream.skip(step.count))});
            } else if (step.op == "stream_read_to_end") {
                EntryStream& stream = require_stream(step.key);
                stream.read_to_end();
                trace.line({"end", "ok"});
            } else if (step.op == "stream_close") {
                EntryStream& stream = require_stream(step.key);
                stream.close();
                trace.line({"close", step.key, "ok"});
            } else if (step.op == "preflight") {
                const auto required = backup.preflight_import_resources(
                    step.conversations, step.settings, step.version, work_dir / step.dest,
                    step.font_limit.value_or(std::numeric_limits<std::int64_t>::max()),
                    available_of(step.available));
                trace.line({"preflight", std::to_string(required)});
            } else if (step.op == "close") {
                backup.close();
                open_archive.reset();
                trace.line({"close", "ok"});
            } else {
                throw std::runtime_error("unknown op: " + step.op);
            }
        } catch (const std::exception& failure) {
            trace.line({"error", failure.what()});
        }
    }
    return trace.str();
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text, std::size_t begin) {
        return text.substr(begin, std::min<std::size_t>(160, text.size() - begin));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual, from)
            << "\n       expected: " << window(expected, from);
    return message.str();
}

}  // namespace

TEST(native_backup_archive_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_NATIVE_BACKUP_ARCHIVE_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    const std::filesystem::path fixtures_dir = root / "fixtures";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));
    CHECK(std::filesystem::is_directory(fixtures_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "agora_native_backup_archive_parity";
    std::filesystem::remove_all(temp_root);
    std::filesystem::create_directories(temp_root);

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario, temp_root, fixtures_dir);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));

    std::filesystem::remove_all(temp_root);
}
