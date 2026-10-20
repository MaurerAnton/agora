// Golden parity test for model/MessagePersistenceGuard.kt.
//
// Replays every scenario under core/tools/golden/message-persistence-guard/scenarios through the
// native guard and requires the trace to equal the frozen Kotlin trace byte for byte. Scenarios
// reach the real MessageSegment model through the backup codec, which is itself pinned against
// kotlinx by the backup-media-policy stand.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/json.hpp"
#include "agora/core/message_persistence_guard.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace backup = agora::core::backup;
namespace guard = agora::core::message_persistence_guard;
using json::Kind;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
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

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

bool is_json_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::string required_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_json_null(value) || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> string_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_json_null(value)) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::optional<int> int_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_json_null(value)) return std::nullopt;
    try {
        return static_cast<int>(std::stoll(value->text));
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " must be an integer");
    }
}

std::vector<backup::MessageSegment> segments_from(const json::Value& root) {
    const json::Value* value = field(root, "segments_json");
    if (is_json_null(value)) return {};
    if (value->kind != Kind::Array) scenario_fail("segments_json must be an array or null");
    const std::optional<std::vector<backup::MessageSegment>> decoded =
        backup::decode_message_segments(json::to_kotlin_string(*value));
    if (!decoded.has_value()) scenario_fail("scenario segments_json does not decode");
    return *decoded;
}

std::string render_trace(const json::Value& root) {
    const std::string name = required_field(root, "name");
    const std::string fn = required_field(root, "fn");
    std::string out = "name|" + esc(name) + "\n";
    out += "fn|" + esc(fn) + "\n";

    if (fn == "clipText") {
        const std::string text = string_field(root, "text").value_or("");
        const std::string result = guard::clip_text(text);
        out += "inputUtf16|" + std::to_string(json::utf16_length(text)) + "\n";
        out += "inputUtf8|" + std::to_string(text.size()) + "\n";
        out += "resultUtf16|" + std::to_string(json::utf16_length(result)) + "\n";
        out += "resultUtf8|" + std::to_string(result.size()) + "\n";
        out += "result|" + esc(result) + "\n";
        return out;
    }

    if (fn != "encodeSegmentsBounded") {
        scenario_fail("unknown function `" + fn + "`");
    }

    const std::vector<backup::MessageSegment> segments = segments_from(root);
    const std::string canonical = backup::encode_message_segments(segments);
    out += "canonical|" + esc(canonical) + "\n";
    out += "canonicalUtf8|" + std::to_string(canonical.size()) + "\n";

    const std::optional<int> max_bytes = int_field(root, "maxBytes");
    try {
        const std::optional<std::string> result =
            max_bytes.has_value() ? guard::encode_segments_bounded(segments, *max_bytes)
                                  : guard::encode_segments_bounded(segments);
        if (!result.has_value()) {
            out += "result|null\n";
        } else {
            out += "resultUtf16|" + std::to_string(json::utf16_length(*result)) + "\n";
            out += "resultUtf8|" + std::to_string(result->size()) + "\n";
            out += "result|" + esc(*result) + "\n";
        }
    } catch (const guard::PersistenceBudgetError& error) {
        out += "error|IllegalStateException|" + esc(error.what()) + "\n";
    }
    return out;
}

void check_scenario(const std::filesystem::path& scenario_path,
                    const std::filesystem::path& golden_dir) {
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value.has_value() || parsed.value->kind != Kind::Object) {
        throw agora::core::test::Failure("scenario must be a JSON object: " +
                                         scenario_path.filename().string());
    }
    const json::Value& scenario = *parsed.value;
    const std::string name = required_field(scenario, "name");
    const std::filesystem::path trace_path = golden_dir / (name + ".trace");
    if (!std::filesystem::is_regular_file(trace_path)) {
        throw agora::core::test::Failure("missing golden trace: " + trace_path.string());
    }
    const std::string expected = read_file(trace_path);
    const std::string actual = render_trace(scenario);
    if (actual != expected) {
        std::size_t offset = 0;
        while (offset < actual.size() && offset < expected.size() &&
               actual[offset] == expected[offset]) {
            ++offset;
        }
        throw agora::core::test::Failure(
            name + ": trace differs at byte " + std::to_string(offset) + " (actual " +
            std::to_string(actual.size()) + " bytes, expected " + std::to_string(expected.size()) +
            ")\nactual:   " + actual.substr(offset, 240) + "\nexpected: " +
            expected.substr(offset, 240));
    }
}

}  // namespace

TEST(message_persistence_guard_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MESSAGE_PERSISTENCE_GUARD_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(scenarios.size() >= 65);
    for (const std::filesystem::path& scenario : scenarios) check_scenario(scenario, golden_dir);
}
