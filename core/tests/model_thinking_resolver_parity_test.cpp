// Golden parity test for data/ModelThinkingCapabilityResolver.kt.
//
// The Kotlin traces are produced from the real resolver, ModelId, CustomProviderConfig and the
// thinking-capability cluster. They pin the parsed provider/model pair, first custom-provider
// match, normalized custom protocol and every capability field.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/model_thinking_resolver.hpp"
#include "check.hpp"

namespace {

namespace resolver = agora::core::model_thinking_resolver;
namespace thinking = agora::core::thinking_capability;
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

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

const Value& require(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string("missing scenario field: ") + std::string(name));
    return *value;
}

std::string required_text(const Value& object, std::string_view name) {
    const Value& value = require(object, name);
    if (value.kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value.text;
}

std::string text_or_empty(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::vector<std::string> string_array(const Value& object, std::string_view name) {
    std::vector<std::string> values;
    const Value* value = field(object, name);
    if (value == nullptr) return values;
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    for (const Value& entry : value->elements) {
        if (entry.kind != Kind::String) {
            scenario_fail(std::string(name) + " must contain strings");
        }
        values.push_back(entry.text);
    }
    return values;
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += esc(values[i]);
    }
    return out;
}

std::vector<resolver::CustomProviderConfig> providers_from(const Value& scenario) {
    std::vector<resolver::CustomProviderConfig> providers;
    const Value* value = field(scenario, "customProviders");
    if (value == nullptr) return providers;
    if (value->kind != Kind::Array) scenario_fail("customProviders must be an array");
    for (const Value& entry : value->elements) {
        if (entry.kind != Kind::Object) scenario_fail("customProviders entries must be objects");
        resolver::CustomProviderConfig provider;
        provider.name = required_text(entry, "name");
        provider.protocol =
            resolver::protocol_from_wire_value(required_text(entry, "protocol"));
        provider.id = text_or_empty(entry, "id");
        provider.legacy_names = string_array(entry, "legacyNames");
        providers.push_back(std::move(provider));
    }
    return providers;
}

std::string render_trace(const Value& scenario) {
    const std::string name = required_text(scenario, "name");
    const std::string selected = required_text(scenario, "selectedModel");
    const std::vector<resolver::CustomProviderConfig> providers = providers_from(scenario);
    const resolver::ModelId parsed = resolver::parse_model_id(selected);
    const thinking::Capability capability =
        resolver::thinking_capability_for_selected_model(selected, providers);

    std::string out = "name|" + esc(name) + "\n";
    out += "selected|" + esc(selected) + "\n";
    out += "parsed|" + esc(parsed.provider_name) + '|' + esc(parsed.model_name) + "\n";
    for (std::size_t i = 0; i < providers.size(); ++i) {
        const resolver::CustomProviderConfig& provider = providers[i];
        out += "provider|" + std::to_string(i) + '|' + esc(provider.name) + '|' +
               std::string(resolver::wire_value(provider.protocol)) + '|' + esc(provider.id) +
               '|' + join(provider.legacy_names) + "\n";
    }
    out += "capability|canDisableThinking|" +
           std::string(capability.can_disable_thinking ? "true" : "false") + "\n";
    out += "capability|supportedEfforts|" + join(capability.supported_efforts) + "\n";
    out += "capability|supportsThinkingBudget|" +
           std::string(capability.supports_thinking_budget ? "true" : "false") + "\n";
    out += "capability|minBudgetTokens|" + std::to_string(capability.min_budget_tokens) + "\n";
    out += "capability|maxBudgetTokens|" +
           (capability.max_budget_tokens.has_value()
                ? std::to_string(*capability.max_budget_tokens)
                : std::string("-")) +
           "\n";
    out += "capability|supportsEffortWithBudget|" +
           std::string(capability.supports_effort_with_budget ? "true" : "false") + "\n";
    out += "capability|supportsSamplingParams|" +
           std::string(capability.supports_sampling_params ? "true" : "false") + "\n";
    out += "capability|supportsEffort|" +
           std::string(capability.supports_effort() ? "true" : "false") + "\n";
    out += "capability|lowestEffort|" +
           (capability.lowest_effort().has_value() ? esc(*capability.lowest_effort())
                                                    : std::string("-")) +
           "\n";
    return out;
}

void check_scenario(const std::filesystem::path& scenario_path,
                    const std::filesystem::path& golden_dir) {
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value.has_value() || parsed.value->kind != Kind::Object) {
        throw agora::core::test::Failure("scenario must be a JSON object: " +
                                         scenario_path.filename().string());
    }
    const Value& scenario = *parsed.value;
    const std::string name = required_text(scenario, "name");
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

TEST(model_thinking_resolver_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MODEL_THINKING_RESOLVER_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(scenarios.size() >= 36);
    for (const std::filesystem::path& scenario : scenarios) check_scenario(scenario, golden_dir);
}
