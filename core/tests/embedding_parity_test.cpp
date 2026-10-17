// Golden parity test for the C++ EmbeddingClient port.
//
// Replays every scenario under core/tools/golden/embedding/scenarios through the C++ port
// (write_embedding_request / write_batch_embedding_request / the response parses) and requires
// the produced trace to equal the frozen Kotlin trace byte for byte. The trace format and mode
// handling mirror the reference driver (core/tools/golden/embedding/EmbeddingTraceDump.kt).
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/embedding.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace embedding = agora::core::embedding;

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

// -- scenario decoding ------------------------------------------------------------------------------

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

bool is_json_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) {
        scenario_fail(std::string(name), "expected a JSON string");
    }
    return value->text;
}

std::string string_or(const json::Value& object, std::string_view name,
                      const std::string& fallback) {
    const auto value = optional_string(object, name);
    return value ? *value : fallback;
}

std::vector<std::string> string_list_or(const json::Value& object, std::string_view name) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return out;
    if (value->kind != json::Kind::Array) scenario_fail(std::string(name), "expected an array");
    for (const json::Value& entry : value->elements) {
        if (entry.kind != json::Kind::String) scenario_fail(std::string(name), "expected strings");
        out.push_back(entry.text);
    }
    return out;
}

// -- scenario runner ---------------------------------------------------------------------------------

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const std::string source = read_file(scenario_path);
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string name = string_or(scenario, "name", stem);
    if (name != stem) {
        scenario_fail(stem, "scenario name '" + name + "' must match the file stem");
    }
    const std::string mode = string_or(scenario, "mode", "single");
    const std::string text = string_or(scenario, "text", "");
    const std::vector<std::string> texts = string_list_or(scenario, "texts");
    const std::string model = string_or(scenario, "model", "text-embedding-3-small");
    const std::optional<std::string> response = optional_string(scenario, "response");

    std::string out;
    const auto line = [&](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& value : fields) {
            if (!first) out += '|';
            first = false;
            out += value;
        }
        out += '\n';
    };
    line({"name", esc(name)});
    line({"mode", esc(mode)});

    if (mode == "requestSingle") {
        line({"body", esc(embedding::write_embedding_request(text, model))});
    } else if (mode == "requestBatch") {
        line({"body", esc(embedding::write_batch_embedding_request(texts, model))});
    } else if (mode == "single") {
        const auto values = embedding::parse_embedding_values(response);
        line({"embedding", "0", values ? *values : std::string("none")});
    } else if (mode == "batch") {
        // computeEmbeddings returns early for empty input, before touching the response.
        if (!texts.empty()) {
            const std::vector<std::optional<std::string>> entries =
                embedding::parse_batch_embedding_entries(response, texts.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                line({"embedding", std::to_string(i),
                      entries[i] ? *entries[i] : std::string("none")});
            }
        }
    } else {
        scenario_fail(stem, "unknown mode: " + mode);
    }
    return out;
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

TEST(embedding_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_EMBEDDING_DIR;
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
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        checked++;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
