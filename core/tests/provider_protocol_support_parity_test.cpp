// Golden parity test for the provider-protocol helpers in data/CustomProviderConfig.kt.
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
#include "agora/core/provider_protocol_support.hpp"
#include "check.hpp"

namespace {

namespace protocol = agora::core::provider_protocol_support;
namespace resolver = agora::core::model_thinking_resolver;
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

std::string text_or(const Value& object, std::string_view name, const std::string& fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

bool bool_or(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a bool");
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    scenario_fail(std::string(name) + " is not a bool");
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

std::vector<protocol::CustomProviderConfig> providers_from(const Value& scenario) {
    std::vector<protocol::CustomProviderConfig> providers;
    const Value* value = field(scenario, "customProviders");
    if (value == nullptr) return providers;
    if (value->kind != Kind::Array) scenario_fail("customProviders must be an array");
    for (const Value& entry : value->elements) {
        if (entry.kind != Kind::Object) scenario_fail("customProviders entries must be objects");
        protocol::CustomProviderConfig provider;
        provider.name = required_text(entry, "name");
        provider.protocol = resolver::protocol_from_wire_value(required_text(entry, "protocol"));
        provider.id = text_or(entry, "id", "");
        provider.legacy_names = string_array(entry, "legacyNames");
        provider.responses_api_enabled = bool_or(entry, "responsesApiEnabled", false);
        provider.anthropic_cache_enabled = bool_or(entry, "anthropicCacheEnabled", true);
        provider.anthropic_cache_ttl = text_or(entry, "anthropicCacheTtl", "1h");
        providers.push_back(std::move(provider));
    }
    return providers;
}

std::string render_trace(const Value& scenario) {
    const std::string name = required_text(scenario, "name");
    const std::string provider = required_text(scenario, "providerName");
    const bool built_in = bool_or(scenario, "builtInEnabled", true);
    const bool built_in_anthropic = bool_or(scenario, "builtInAnthropicEnabled", true);
    const std::string built_in_ttl = text_or(scenario, "builtInTtl", "1h");
    const std::vector<protocol::CustomProviderConfig> providers = providers_from(scenario);

    std::string out = "name|" + esc(name) + "\n";
    out += "provider|" + esc(provider) + "\n";
    out += "builtInEnabled|" + std::string(built_in ? "true" : "false") + "\n";
    out += "builtInAnthropicEnabled|" +
           std::string(built_in_anthropic ? "true" : "false") + "\n";
    out += "builtInTtl|" + esc(built_in_ttl) + "\n";
    for (std::size_t i = 0; i < providers.size(); ++i) {
        const protocol::CustomProviderConfig& config = providers[i];
        out += "custom|" + std::to_string(i) + '|' + esc(config.name) + '|' +
               std::string(resolver::wire_value(config.protocol)) + '|' + esc(config.id) + '|' +
               join(config.legacy_names) + '|' +
               (config.responses_api_enabled ? "true" : "false") + '|' +
               (config.anthropic_cache_enabled ? "true" : "false") + '|' +
               esc(config.anthropic_cache_ttl) + "\n";
    }
    out += "isOpenAiProtocol|" +
           std::string(protocol::is_open_ai_protocol_provider(provider, providers) ? "true" : "false") +
           "\n";
    out += "isAnthropicProtocol|" +
           std::string(protocol::is_anthropic_protocol_provider(provider, providers) ? "true"
                                                                                    : "false") +
           "\n";
    out += "isAnthropicCacheEnabled|" +
           std::string(protocol::is_anthropic_cache_enabled_for_provider(provider, built_in,
                                                                        providers)
                           ? "true"
                           : "false") +
           "\n";
    out += "supportsPromptCache|" +
           std::string(protocol::supports_prompt_cache_setting(provider, providers) ? "true"
                                                                                   : "false") +
           "\n";
    out += "isPromptCacheKeyEnabled|" +
           std::string(protocol::is_prompt_cache_key_enabled_for_provider(provider, providers)
                           ? "true"
                           : "false") +
           "\n";
    out += "isPromptCacheSettingEnabled|" +
           std::string(protocol::is_prompt_cache_setting_enabled_for_provider(
                           provider, built_in_anthropic, providers)
                           ? "true"
                           : "false") +
           "\n";
    out += "anthropicCacheTtl|" +
           esc(protocol::anthropic_cache_ttl_for_provider(provider, built_in_ttl, providers)) + "\n";
    out += "isResponsesApiEnabled|" +
           std::string(protocol::is_responses_api_enabled_for_provider(provider, built_in,
                                                                       providers)
                           ? "true"
                           : "false") +
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

TEST(provider_protocol_support_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_PROVIDER_PROTOCOL_SUPPORT_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(scenarios.size() >= 27);
    for (const std::filesystem::path& scenario : scenarios) check_scenario(scenario, golden_dir);
}
