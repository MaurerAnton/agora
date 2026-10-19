// Golden parity test for the native thinking-capability cluster.
//
// Replays every scenario under core/tools/golden/thinking-capability/scenarios through
// agora::core::thinking_capability and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/thinking-capability/ThinkingCapabilityTraceDump.kt).
//
// The pins that matter: the normalization vocabulary (including legacy `budget:` migration and
// Kotlin whitespace), the protocol-over-name family resolution, every documented per-model
// refinement (effort subsets, budget floors/ceilings, effort-with-budget support, sampling), the
// nearest-effort rule (ties resolve upward, unknown values fall to the first accepted level), and
// the forced-thinking-off fallback used by compact/title/transcription callers.
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
#include "agora/core/thinking_capability.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace tc = agora::core::thinking_capability;
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

/** The driver's optOrDash: null is "-", every real string is escaped (empty stays empty). */
std::string opt(const std::optional<std::string>& value) {
    return value.has_value() ? esc(*value) : "-";
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::optional<std::int64_t> number(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind == Kind::Array || value->kind == Kind::Object) {
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

bool flag(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return false;
    return value->kind == Kind::Literal && value->text == "true";
}

tc::ThinkingProviderFamily family_of(std::string_view name) {
    if (name == "ANTHROPIC") return tc::ThinkingProviderFamily::Anthropic;
    if (name == "OPENAI") return tc::ThinkingProviderFamily::OpenAI;
    if (name == "OPENAI_COMPATIBLE") return tc::ThinkingProviderFamily::OpenAICompatible;
    if (name == "OPENROUTER") return tc::ThinkingProviderFamily::OpenRouter;
    if (name == "DEEPSEEK") return tc::ThinkingProviderFamily::DeepSeek;
    if (name == "QWEN") return tc::ThinkingProviderFamily::Qwen;
    if (name == "GROQ") return tc::ThinkingProviderFamily::Groq;
    if (name == "GEMINI") return tc::ThinkingProviderFamily::Gemini;
    if (name == "OLLAMA") return tc::ThinkingProviderFamily::Ollama;
    if (name == "LOCAL") return tc::ThinkingProviderFamily::Local;
    scenario_fail("unknown family: " + std::string(name));
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = required_text(root, "op");

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";

    if (op == "normalize") {
        const std::optional<std::string> raw = optional_text(root, "raw");
        trace += "args|" + opt(raw) + "\n";
        trace += "result|" + esc(tc::normalize(raw)) + "\n";
    } else if (op == "legacyBudget") {
        const std::optional<std::string> raw = optional_text(root, "raw");
        const std::optional<int> result = tc::legacy_budget_tokens(raw);
        trace += "args|" + opt(raw) + "\n";
        trace += "result|" + (result.has_value() ? std::to_string(*result) : "null") + "\n";
    } else if (op == "effortForIndex") {
        const int index = static_cast<int>(number(root, "index").value_or(0));
        trace += "args|" + std::to_string(index) + "\n";
        trace += "result|" + esc(tc::effort_for_index(index)) + "\n";
    } else if (op == "indexForEffort") {
        const std::string effort = required_text(root, "effort");
        trace += "args|" + esc(effort) + "\n";
        trace += "result|" + std::to_string(tc::index_for_effort(effort)) + "\n";
    } else if (op == "family") {
        const std::optional<std::string> provider = optional_text(root, "providerName");
        const std::optional<std::string> protocol = optional_text(root, "customProtocol");
        trace += "args|" + opt(provider) + "|" + opt(protocol) + "\n";
        trace += "result|" + esc(tc::describe(tc::family_for(provider, protocol))) + "\n";
    } else if (op == "familyDefault") {
        const std::string family_name = required_text(root, "family");
        trace += "args|" + esc(family_name) + "\n";
        trace += "result|" + esc(tc::describe(tc::family_default(family_of(family_name)))) + "\n";
    } else if (op == "capability") {
        const std::string family_name = required_text(root, "family");
        const std::string model_id = required_text(root, "modelId");
        trace += "args|" + esc(family_name) + "|" + esc(model_id) + "\n";
        trace += "result|" + esc(tc::describe(tc::for_model(family_of(family_name), model_id))) + "\n";
    } else if (op == "providerCapability") {
        const std::optional<std::string> provider = optional_text(root, "providerName");
        const std::optional<std::string> protocol = optional_text(root, "customProtocol");
        const std::string model_id = required_text(root, "modelId");
        trace += "args|" + opt(provider) + "|" + opt(protocol) + "|" + esc(model_id) + "\n";
        trace += "result|" +
                 esc(tc::describe(tc::for_provider(provider, model_id, protocol))) + "\n";
    } else if (op == "nearest") {
        const std::string family_name = required_text(root, "family");
        const std::string model_id = required_text(root, "modelId");
        const std::string effort = required_text(root, "effort");
        trace += "args|" + esc(family_name) + "|" + esc(model_id) + "|" + esc(effort) + "\n";
        const tc::Capability capability = tc::for_model(family_of(family_name), model_id);
        trace += "result|" + opt(tc::nearest_effort(capability, effort)) + "\n";
    } else if (op == "clamp") {
        const std::string family_name = required_text(root, "family");
        const std::string model_id = required_text(root, "modelId");
        const int tokens = static_cast<int>(number(root, "tokens").value_or(0));
        trace += "args|" + esc(family_name) + "|" + esc(model_id) + "|" + std::to_string(tokens) +
                 "\n";
        const tc::Capability capability = tc::for_model(family_of(family_name), model_id);
        trace += "result|" + std::to_string(tc::clamp_budget(capability, tokens)) + "\n";
    } else if (op == "resolve") {
        const std::string family_name = required_text(root, "family");
        const std::string model_id = required_text(root, "modelId");
        const bool enabled = flag(root, "enabled");
        const std::string effort = required_text(root, "effort");
        const bool budget_enabled = flag(root, "budgetEnabled");
        const int budget_tokens = static_cast<int>(number(root, "budgetTokens").value_or(0));
        trace += "args|" + esc(family_name) + "|" + esc(model_id) + "|" +
                 (enabled ? "true" : "false") + "|" + esc(effort) + "|" +
                 (budget_enabled ? "true" : "false") + "|" + std::to_string(budget_tokens) + "\n";
        const tc::Capability capability = tc::for_model(family_of(family_name), model_id);
        const tc::ResolvedThinking resolved =
            tc::resolve(capability, enabled, effort, budget_enabled, budget_tokens);
        trace += "result|" + esc(tc::describe(resolved)) + "\n";
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

TEST(thinking_capability_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_THINKING_CAPABILITY_DIR;
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
