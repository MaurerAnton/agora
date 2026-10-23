// Golden parity test for NativeBackupV5Baseline + NativeBackupV5Writer against the Kotlin
// reference (data/NativeBackupV5Writer.kt and data/NativeBackupV5Baseline.kt).
//
// Each scenario replays the frozen trace step for step: entry assembly, baseline manifest/index
// validation, reuse decisions over intact and damaged baselines, raw copies that keep the
// compressed stream untouched, and the full archive dump (name, content, CRC, size, method) read
// back through the already-pinned zip reader.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/native_backup_v5.hpp"
#include "agora/core/zip_reader.hpp"
#include "agora/core/zip_writer.hpp"
#include "check.hpp"

namespace {

namespace fs = std::filesystem;
namespace json = agora::core::json;
namespace v5 = agora::core::native_backup_v5;
namespace zip = agora::core::zip;

using json::Kind;
using json::Member;
using json::Value;

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

const Value& required(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail("missing field " + std::string(name));
    return *value;
}

std::string required_string(const Value& object, std::string_view name) {
    const Value& value = required(object, name);
    if (value.kind != Kind::String) {
        scenario_fail("field " + std::string(name) + " must be a string");
    }
    return value.text;
}

std::int64_t required_long(const Value& object, std::string_view name) {
    const Value& value = required(object, name);
    if (value.kind != Kind::Literal) {
        scenario_fail("field " + std::string(name) + " must be a number");
    }
    try {
        return std::stoll(value.text);
    } catch (const std::exception&) {
        scenario_fail("field " + std::string(name) + " is not a number");
    }
}

std::optional<bool> optional_bool(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Literal) return std::nullopt;
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return std::nullopt;
}

std::optional<std::string> optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) return std::nullopt;
    return value->text;
}

std::vector<std::string> string_array(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    std::vector<std::string> result;
    if (value == nullptr) return result;
    if (value->kind != Kind::Array) {
        scenario_fail("field " + std::string(name) + " must be an array");
    }
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) {
            scenario_fail("field " + std::string(name) + " must hold strings");
        }
        result.push_back(element.text);
    }
    return result;
}

const std::vector<Value>& array_elements(const Value& object, std::string_view name) {
    static const std::vector<Value> kEmpty;
    const Value* value = field(object, name);
    if (value == nullptr) return kEmpty;
    if (value->kind != Kind::Array) {
        scenario_fail("field " + std::string(name) + " must be an array");
    }
    return value->elements;
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

// --------------------------------------------------------------------- scenario state

class ScenarioState {
public:
    ScenarioState() = default;

    void reset() {
        std::error_code error;
        fs::remove_all(workspace_, error);
        fs::create_directories(workspace_, error);
        outputs_.clear();
        dump_files_.clear();
        copied_media_.clear();
    }

    void cleanup() {
        std::error_code error;
        fs::remove_all(workspace_, error);
    }

    fs::path file(const std::string& key) const { return workspace_ / key; }

    zip::Writer& output_stream(const std::string& key) {
        if (outputs_.find(key) == outputs_.end()) {
            dump_files_.emplace(key, true);
            outputs_.emplace(key, zip::Writer{});
        }
        return outputs_.at(key);
    }

    void close_outputs() {
        for (auto& [key, writer] : outputs_) {
            write_file(file(key), writer.finish());
        }
        outputs_.clear();
    }

    std::map<std::string, bool> dump_files_;
    std::map<std::string, std::vector<std::string>> copied_media_;

private:
    fs::path workspace_ = "v5_workspace";
    std::map<std::string, zip::Writer> outputs_;
};

// --------------------------------------------------------------------- JSON shapes

v5::ConversationIndexEntry entry_of(const Value& source) {
    v5::ConversationIndexEntry entry;
    entry.id = required_string(source, "id");
    entry.data_changed_at = required_long(source, "dataChangedAt");
    entry.entry = required_string(source, "entry");
    entry.media_entries = string_array(source, "mediaEntries");
    return entry;
}

std::string entry_json(const v5::ConversationIndexEntry& entry) {
    std::string out = "{\"id\":";
    append_quoted(out, entry.id);
    out += ",\"dataChangedAt\":";
    out += std::to_string(entry.data_changed_at);
    out += ",\"entry\":";
    append_quoted(out, entry.entry);
    out += ",\"mediaEntries\":[";
    bool first = true;
    for (const std::string& media : entry.media_entries) {
        if (!first) out.push_back(',');
        first = false;
        append_quoted(out, media);
    }
    out += "]}";
    return out;
}

std::string error_json(std::string_view type, std::string_view message) {
    std::string out = "{\"error\":";
    append_quoted(out, type);
    out += ",\"message\":";
    append_quoted(out, message);
    out.push_back('}');
    return out;
}

// --------------------------------------------------------------------- operations

std::string create_baseline(const Value& step, ScenarioState& state) {
    const std::string key = required_string(step, "file");
    state.dump_files_[key] = optional_bool(step, "dump").value_or(true);

    zip::Writer zip;
    std::string manifest_text;
    if (const auto text = optional_string(step, "manifestText")) {
        manifest_text = *text;
    } else {
        manifest_text = json::to_kotlin_string(required(step, "manifest"));
    }
    zip.put_entry("manifest.json", manifest_text);

    for (const Value& media : array_elements(step, "media")) {
        zip.put_entry(required_string(media, "name"), required_string(media, "content"));
    }

    std::vector<v5::ConversationIndexEntry> derived;
    for (const Value& item : array_elements(step, "items")) {
        derived.push_back(v5::write_conversation_entry(
            zip, required_string(item, "id"), required_long(item, "dataChangedAt"),
            required_string(item, "conversation"), string_array(item, "runs"),
            string_array(item, "messages"), string_array(item, "loops"),
            string_array(item, "mediaEntries")));
    }

    std::string index_text;
    if (const auto text = optional_string(step, "indexText")) {
        index_text = *text;
    } else {
        v5::ConversationIndex document;
        if (const Value* explicit_index = field(step, "index")) {
            if (explicit_index->kind != Kind::Array) scenario_fail("index must be an array");
            for (const Value& element : explicit_index->elements) {
                document.conversations.push_back(entry_of(element));
            }
        } else {
            document.conversations = derived;
        }
        index_text = v5::encode_index(document);
    }
    zip.put_entry("conv/index.json", index_text);

    write_file(state.file(key), zip.finish());
    return "null";
}

std::string open_or_null(const Value& step, ScenarioState& state) {
    const auto baseline = v5::Baseline::open_or_null(state.file(required_string(step, "file")));
    if (!baseline.has_value()) return "null";
    std::string out = "{\"index\":{\"conversations\":[";
    bool first = true;
    for (const auto& item : baseline->index().conversations) {
        if (!first) out.push_back(',');
        first = false;
        out += entry_json(item);
    }
    out += "]}}";
    return out;
}

std::string can_reuse(const Value& step, ScenarioState& state) {
    auto baseline = v5::Baseline::open_or_null(state.file(required_string(step, "file")));
    if (!baseline.has_value()) return "null";
    const bool reusable =
        baseline->can_reuse(required_string(step, "id"), required_long(step, "dataChangedAt"));
    return reusable ? "true" : "false";
}

std::string index_entry(const Value& step, ScenarioState& state) {
    auto baseline = v5::Baseline::open_or_null(state.file(required_string(step, "file")));
    if (!baseline.has_value()) return "null";
    const v5::ConversationIndexEntry* entry =
        baseline->index_entry(required_string(step, "id"));
    return entry == nullptr ? "null" : entry_json(*entry);
}

std::string write_conversation_entry(const Value& step, ScenarioState& state) {
    zip::Writer& zip = state.output_stream(required_string(step, "file"));
    const v5::ConversationIndexEntry entry = v5::write_conversation_entry(
        zip, required_string(step, "conversationId"), required_long(step, "dataChangedAt"),
        required_string(step, "conversation"), string_array(step, "runs"),
        string_array(step, "messages"), string_array(step, "loops"),
        string_array(step, "mediaEntries"));
    return entry_json(entry);
}

std::string write_tasks_entry(const Value& step, ScenarioState& state) {
    zip::Writer& zip = state.output_stream(required_string(step, "file"));
    v5::write_tasks_entry(zip, string_array(step, "tasks"));
    return "null";
}

std::string write_conversation_index(const Value& step, ScenarioState& state) {
    zip::Writer& zip = state.output_stream(required_string(step, "file"));
    std::vector<v5::ConversationIndexEntry> entries;
    for (const Value& element : array_elements(step, "entries")) {
        entries.push_back(entry_of(element));
    }
    v5::write_conversation_index(zip, entries);
    return "null";
}

std::string copy_conversation_from_baseline(const Value& step, ScenarioState& state) {
    const std::string file = required_string(step, "file");
    zip::Writer& zip = state.output_stream(file);
    auto baseline = v5::Baseline::open_or_null(state.file(required_string(step, "baseline")));
    if (!baseline.has_value()) return "null";

    std::vector<std::string>& copied = state.copied_media_[file];
    if (const Value* seeded = field(step, "copiedMedia")) {
        if (seeded->kind != Kind::Array) scenario_fail("copiedMedia must be an array");
        for (const Value& element : seeded->elements) {
            if (element.kind != Kind::String) scenario_fail("copiedMedia must hold strings");
            if (std::find(copied.begin(), copied.end(), element.text) == copied.end()) {
                copied.push_back(element.text);
            }
        }
    }

    const v5::ConversationIndexEntry entry = v5::copy_conversation_from_baseline(
        zip, *baseline, entry_of(required(step, "entry")), copied);

    std::string out = "{\"entry\":";
    out += entry_json(entry);
    out += ",\"copiedMedia\":[";
    bool first = true;
    for (const std::string& media : copied) {
        if (!first) out.push_back(',');
        first = false;
        append_quoted(out, media);
    }
    out += "]}";
    return out;
}

std::string copy_raw(const Value& step, ScenarioState& state) {
    auto baseline = v5::Baseline::open_or_null(state.file(required_string(step, "baseline")));
    if (!baseline.has_value()) return "null";
    const std::string name = required_string(step, "name");
    zip::Writer& zip = state.output_stream(required_string(step, "file"));
    baseline->copy_raw(name, zip);
    return "null";
}

/** Flips the stored CRC of a named entry in both the local header and the central directory. */
void corrupt_entry_crc(const fs::path& path, const std::string& name) {
    std::string bytes = read_file(path);
    auto u16 = [&](std::size_t at) -> std::uint16_t {
        return static_cast<std::uint16_t>(
            static_cast<unsigned char>(bytes[at]) |
            (static_cast<unsigned char>(bytes[at + 1]) << 8));
    };
    auto u32 = [&](std::size_t at) -> std::uint32_t {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
    };
    auto name_at = [&](std::size_t at, std::size_t length) -> std::string {
        if (at + length > bytes.size()) return std::string();
        return bytes.substr(at, length);
    };
    auto patch = [&](std::size_t at) {
        const std::uint32_t flipped = u32(at) ^ 0xFFFFFFFFU;
        bytes[at] = static_cast<char>(flipped & 0xFF);
        bytes[at + 1] = static_cast<char>((flipped >> 8) & 0xFF);
        bytes[at + 2] = static_cast<char>((flipped >> 16) & 0xFF);
        bytes[at + 3] = static_cast<char>((flipped >> 24) & 0xFF);
    };
    for (std::size_t offset = 0; offset + 4 <= bytes.size(); ++offset) {
        const std::uint32_t signature = u32(offset);
        if (signature == 0x04034B50U) {  // local file header
            const std::size_t name_length = u16(offset + 26);
            if (name_at(offset + 30, name_length) == name) patch(offset + 14);
        }
        if (signature == 0x02014B50U) {  // central directory header
            const std::size_t name_length = u16(offset + 28);
            if (name_at(offset + 46, name_length) == name) patch(offset + 16);
        }
    }
    write_file(path, bytes);
}

std::string corrupt_entry(const Value& step, ScenarioState& state) {
    const std::string key = required_string(step, "file");
    state.dump_files_[key] = false;
    corrupt_entry_crc(state.file(key), required_string(step, "name"));
    return "null";
}

std::string execute_operation(const std::string& op, const Value& step, ScenarioState& state) {
    if (op == "createBaseline") return create_baseline(step, state);
    if (op == "openOrNull") return open_or_null(step, state);
    if (op == "canReuse") return can_reuse(step, state);
    if (op == "indexEntry") return index_entry(step, state);
    if (op == "writeConversationEntry") return write_conversation_entry(step, state);
    if (op == "writeTasksEntry") return write_tasks_entry(step, state);
    if (op == "writeConversationIndex") return write_conversation_index(step, state);
    if (op == "copyConversationFromBaseline") return copy_conversation_from_baseline(step, state);
    if (op == "copyRaw") return copy_raw(step, state);
    if (op == "corruptEntry") return corrupt_entry(step, state);
    scenario_fail("unknown operation " + op);
}

// --------------------------------------------------------------------- archive dump

std::string dump_archive(const fs::path& path) {
    if (!fs::is_regular_file(path)) return "[]";
    zip::Reader reader(path.string());
    std::string out = "[";
    bool first = true;
    for (const zip::CentralEntry& entry : reader.central_entries()) {
        const std::string content = reader.read_at(entry.local_offset, entry.size, entry.crc32);
        if (!first) out.push_back(',');
        first = false;
        out += "{\"name\":";
        append_quoted(out, entry.name);
        out += ",\"content\":";
        append_quoted(out, content);
        out += ",\"crc\":";
        out += std::to_string(static_cast<std::uint64_t>(entry.crc32));
        out += ",\"size\":";
        out += std::to_string(entry.size);
        out += ",\"method\":";
        out += std::to_string(entry.method);
        out.push_back('}');
    }
    out.push_back(']');
    return out;
}

// --------------------------------------------------------------------- scenario runner

std::string run_scenario(const fs::path& scenario_path) {
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value.has_value()) scenario_fail("scenario is not valid JSON");
    const Value& root = *parsed.value;

    ScenarioState state;
    state.reset();
    try {
        std::string steps = "[";
        bool first_step = true;
        for (const Value& element : array_elements(root, "steps")) {
            if (!first_step) steps.push_back(',');
            first_step = false;
            const std::string op = required_string(element, "op");
            std::string result;
            try {
                result = execute_operation(op, element, state);
            } catch (const v5::IOException& error) {
                result = error_json("IOException", error.what());
            } catch (const std::exception& error) {
                result = error_json("Exception", error.what());
            }
            steps += "{\"op\":";
            append_quoted(steps, op);
            steps += ",\"result\":";
            steps += result;
            steps.push_back('}');
        }
        steps.push_back(']');
        state.close_outputs();

        std::string archives = "{";
        bool first_archive = true;
        for (const auto& [key, enabled] : state.dump_files_) {
            if (!enabled) continue;
            if (!first_archive) archives.push_back(',');
            first_archive = false;
            append_quoted(archives, key);
            archives.push_back(':');
            archives += dump_archive(state.file(key));
        }
        archives.push_back('}');

        std::string trace = "{\"steps\":";
        trace += steps;
        trace += ",\"archives\":";
        trace += archives;
        trace.push_back('}');
        state.cleanup();
        return trace;
    } catch (...) {
        state.cleanup();
        throw;
    }
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t common = 0;
    while (common < actual.size() && common < expected.size() &&
           actual[common] == expected[common]) {
        ++common;
    }
    const std::size_t start = common > 80 ? common - 80 : 0;
    std::string snippet_actual = actual.substr(start, 200);
    std::string snippet_expected = expected.substr(start, 200);
    return "trace mismatch at byte " + std::to_string(common) + "\n  expected: " +
           snippet_expected + "\n  actual:   " + snippet_actual;
}

}  // namespace

TEST(native_backup_v5_parity) {
    const fs::path root = AGORA_FIXTURE_NATIVE_BACKUP_V5_DIR;
    const fs::path scenarios_dir = root / "scenarios";
    const fs::path golden_dir = root / "golden";
    CHECK(fs::is_directory(scenarios_dir));
    CHECK(fs::is_directory(golden_dir));

    std::vector<fs::path> scenarios;
    for (const auto& entry : fs::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const fs::path& scenario : scenarios) {
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
        CHECK(!expected.empty());
        std::string actual;
        try {
            actual = run_scenario(scenario);
        } catch (const std::exception& error) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " + error.what());
        }
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
