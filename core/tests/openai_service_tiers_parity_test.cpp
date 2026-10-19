// Golden parity test for the OpenAI service-tier policy.
//
// Replays every scenario under core/tools/golden/openai-service-tiers/scenarios against the
// shipped port and requires the produced trace to equal the frozen Kotlin trace byte for byte.
// The trace format mirrors the reference driver
// (core/tools/golden/openai-service-tiers/OpenAiServiceTiersTraceDump.kt).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_service_tiers.hpp"
#include "check.hpp"

namespace {

namespace tiers = agora::core::openai_service_tiers;
namespace json = agora::core::json;

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

std::string opt(const std::optional<std::string>& value) {
    return value.has_value() ? esc(*value) : "-";
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == json::Kind::Literal && value->text == "null");
}

std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::optional<std::int64_t> number(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind == json::Kind::Array || value->kind == json::Kind::Object) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value->text, &consumed);
        if (consumed != value->text.size()) return std::nullopt;
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool flag(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    return value->kind == json::Kind::Literal && value->text == "true";
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ",";
        out += items[i];
    }
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem + ": not an object");
    }
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = required_text(root, "op");

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";

    if (op == "normalize") {
        const std::optional<std::string> value = optional_text(root, "value");
        trace += "args|" + opt(value) + "\n";
        trace += "result|" + esc(tiers::normalize(value)) + "\n";
    } else if (op == "available") {
        const std::optional<std::string> model = optional_text(root, "model");
        const bool official = flag(root, "official", true);
        trace += "args|" + opt(model) + "|" + bool_text(official) + "\n";
        trace += "result|" + esc(join(tiers::available_tiers(model, official))) + "\n";
    } else if (op == "mapped") {
        const std::optional<std::string> value = optional_text(root, "value");
        const std::optional<std::string> model = optional_text(root, "model");
        const bool official = flag(root, "official", true);
        trace += "args|" + opt(value) + "|" + opt(model) + "|" + bool_text(official) + "\n";
        trace += "result|" + esc(tiers::mapped_tier(value, model, official)) + "\n";
    } else if (op == "index") {
        const std::optional<std::string> value = optional_text(root, "value");
        trace += "args|" + opt(value) + "\n";
        trace += "result|" + std::to_string(tiers::index_for_tier(value)) + "\n";
    } else if (op == "tier") {
        const int index = static_cast<int>(number(root, "index").value_or(0));
        trace += "args|" + std::to_string(index) + "\n";
        trace += "result|" + esc(tiers::tier_for_index(index)) + "\n";
    } else if (op == "request") {
        const bool enabled = flag(root, "enabled", false);
        const std::optional<std::string> value = optional_text(root, "value");
        const bool responses = flag(root, "responses", false);
        const std::optional<std::string> model = optional_text(root, "model");
        const bool official = flag(root, "official", true);
        trace += "args|" + bool_text(enabled) + "|" + opt(value) + "|" + bool_text(responses) +
                 "|" + opt(model) + "|" + bool_text(official) + "\n";
        trace += "result|" +
                 opt(tiers::request_value(enabled, value, responses, model, official)) + "\n";
    } else {
        scenario_fail(stem + ": unknown op " + op);
    }
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(200, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(openai_service_tiers_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_OPENAI_SERVICE_TIERS_DIR;
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
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
