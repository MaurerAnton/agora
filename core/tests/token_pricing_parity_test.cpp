// Golden parity test for the token-pricing cluster.
//
// Replays every scenario under core/tools/golden/token-pricing/scenarios against the shipped
// implementation and requires the produced trace to equal the frozen Kotlin trace byte for byte.
// The trace format mirrors the reference driver
// (core/tools/golden/token-pricing/TokenPricingTraceDump.kt).
//
// The pricing rules themselves live in the message-preparation port
// (agora::core::prep, src/message_preparation_tokens.cpp) — a second implementation here would
// only be a place for the two to drift. This stand is the *wider* net over that code: 154 traces
// against the message-preparation stand's own subset, covering the boundaries that module's
// golden does not reach (NBSP classification, id-regex edges, envelope fallbacks, clamp edges,
// shrink correction on non-square inputs). The genuinely missing piece,
// LocalModelTextCounter.kt, is ported in agora::core::token_pricing and pinned by the `local`
// scenarios here.
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

#include "agora/core/message_preparation.hpp"
#include "agora/core/token_pricing.hpp"
#include "check.hpp"

namespace {

namespace prep = agora::core::prep;
namespace tp = agora::core::token_pricing;
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

std::string opt(const std::optional<int>& value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

std::string opt(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

/** Kotlin prints `null`, not the driver's `-` placeholder: data-class fields use this. */
std::string null_text(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

std::string null_text(const std::optional<int>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

/** Kotlin data-class toString() for the types this cluster prints. */
std::string describe(const prep::ImageDescriptor& image) {
    return "ImageDescriptor(byteSize=" + null_text(image.byte_size) +
           ", pixelWidth=" + null_text(image.pixel_width) +
           ", pixelHeight=" + null_text(image.pixel_height) + ")";
}

std::string describe(const prep::AnthropicModelId& model) {
    const char* tier = "null";
    if (model.tier.has_value()) {
        switch (*model.tier) {
            case prep::AnthropicModelId::Tier::Opus: tier = "OPUS"; break;
            case prep::AnthropicModelId::Tier::Sonnet: tier = "SONNET"; break;
            case prep::AnthropicModelId::Tier::Haiku: tier = "HAIKU"; break;
        }
    }
    return "AnthropicModelId(tier=" + std::string(tier) +
           ", major=" + null_text(model.major) + ", minor=" + std::to_string(model.minor) + ")";
}

std::string describe(const prep::EnvelopeCost& envelope) {
    return "EnvelopeCost(perMessage=" + std::to_string(envelope.per_message) +
           ", perToolDefinition=" + std::to_string(envelope.per_tool_definition) +
           ", perToolCall=" + std::to_string(envelope.per_tool_call) +
           ", requestOverhead=" + std::to_string(envelope.request_overhead) +
           ", toolSetOverhead=" + std::to_string(envelope.tool_set_overhead) + ")";
}

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
    return value == nullptr ||
           (value->kind == json::Kind::Literal && value->text == "null");
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

bool flag(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return false;
    return value->kind == json::Kind::Literal && value->text == "true";
}

prep::ImageDescriptor read_image(const json::Value& root) {
    const json::Value* image = field(root, "image");
    if (image == nullptr || image->kind != json::Kind::Object) {
        scenario_fail("image must be an object");
    }
    prep::ImageDescriptor descriptor;
    if (const auto bytes = number(*image, "byteSize")) descriptor.byte_size = *bytes;
    if (const auto width = number(*image, "pixelWidth")) {
        descriptor.pixel_width = static_cast<std::int32_t>(*width);
    }
    if (const auto height = number(*image, "pixelHeight")) {
        descriptor.pixel_height = static_cast<std::int32_t>(*height);
    }
    return descriptor;
}

/** The family branch of cost_model_for without depending on a tokenizer: pricing only. */
prep::EnvelopeCost family_envelope(std::string provider, std::string model) {
    const prep::ContextCostModel costs = prep::cost_model_for(
        prep::PrefixedModelId{std::move(provider), std::move(model)},
        [](std::string_view) { return std::int64_t{0}; });
    return costs.envelope;
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

    if (op == "heuristic") {
        const std::string text = required_text(root, "text");
        trace += "args|" + esc(text) + "\n";
        trace += "result|" + std::to_string(prep::heuristic_count(text)) + "\n";
    } else if (op == "local") {
        const std::string text = required_text(root, "text");
        std::optional<int> exact;
        if (const auto value = number(root, "exact")) exact = static_cast<int>(*value);
        const bool throws = flag(root, "throws");
        trace += "args|" + esc(text) + "|" + opt(exact) + "|" + bool_text(throws) + "\n";
        trace += "result|" + std::to_string(tp::local_model_text_tokens(text, exact, throws)) +
                 "\n";
    } else if (op == "byteImage") {
        const prep::ImageDescriptor image = read_image(root);
        trace += "args|" + opt(image.byte_size) + "|" + opt(image.pixel_width) + "|" +
                 opt(image.pixel_height) + "\n";
        trace += "result|" + std::to_string(prep::byte_proportional_image_tokens(image)) + "\n";
    } else if (op == "anthropicTier") {
        const std::string model = required_text(root, "model");
        trace += "args|" + esc(model) + "\n";
        trace += "result|";
        trace += prep::anthropic_high_resolution_for_model(model) ? "HIGH_RESOLUTION" : "STANDARD";
        trace += "\n";
    } else if (op == "anthropicImage") {
        const std::string model = required_text(root, "model");
        const prep::ImageDescriptor image = read_image(root);
        trace += "args|" + esc(model) + "|" + esc(describe(image)) + "\n";
        trace += "result|" +
                 std::to_string(prep::anthropic_image_tokens(
                     image, prep::anthropic_high_resolution_for_model(model))) +
                 "\n";
    } else if (op == "geminiImage") {
        const prep::ImageDescriptor image = read_image(root);
        trace += "args|" + esc(describe(image)) + "\n";
        trace += "result|" + std::to_string(prep::gemini_image_tokens(image)) + "\n";
    } else if (op == "openaiImage") {
        const std::string model = required_text(root, "model");
        const prep::ImageDescriptor image = read_image(root);
        trace += "args|" + esc(model) + "|" + esc(describe(image)) + "\n";
        trace += "result|" +
                 std::to_string(prep::openai_image_tokens_for_model(image, model)) + "\n";
    } else if (op == "anthropicModelId") {
        const std::string model = required_text(root, "model");
        trace += "args|" + esc(model) + "\n";
        trace += "result|" + esc(describe(prep::parse_anthropic_model_id(model))) + "\n";
    } else if (op == "versionAtLeast") {
        const std::string model = required_text(root, "model");
        const int major = static_cast<int>(number(root, "major").value_or(0));
        const int minor = static_cast<int>(number(root, "minor").value_or(0));
        trace += "args|" + esc(model) + "|" + std::to_string(major) + "|" +
                 std::to_string(minor) + "\n";
        trace += "result|" +
                 bool_text(prep::anthropic_version_at_least(
                     prep::parse_anthropic_model_id(model), major, minor)) +
                 "\n";
    } else if (op == "envelopeDefault") {
        trace += "args|\n";
        trace += "result|" + esc(describe(prep::ContextCostModel::default_model().envelope)) +
                 "\n";
    } else if (op == "envelopeOpenAi") {
        trace += "args|\n";
        trace += "result|" + esc(describe(family_envelope("openai", "gpt-4o"))) + "\n";
    } else if (op == "envelopeAnthropic") {
        const std::string model = required_text(root, "model");
        trace += "args|" + esc(model) + "\n";
        trace += "result|" + esc(describe(family_envelope("anthropic", model))) + "\n";
    } else if (op == "margin") {
        const std::int64_t raw = number(root, "raw").value_or(0);
        trace += "args|" + std::to_string(raw) + "\n";
        trace += "result|" +
                 std::to_string(prep::ContextCostModel::default_model().safety_margin.apply(raw)) +
                 "\n";
    } else if (op == "defaultModel") {
        const std::string text = required_text(root, "text");
        const prep::ImageDescriptor image = read_image(root);
        const std::int64_t raw = number(root, "raw").value_or(0);
        trace += "args|" + esc(text) + "|" + esc(describe(image)) + "|" +
                 std::to_string(raw) + "\n";
        const prep::ContextCostModel model = prep::ContextCostModel::default_model();
        trace += "result|" + std::to_string(model.text(text)) + "|" +
                 std::to_string(model.image(image)) + "|" + esc(describe(model.envelope)) + "|" +
                 std::to_string(model.safety_margin.apply(raw)) + "\n";
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

TEST(token_pricing_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_TOKEN_PRICING_DIR;
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
