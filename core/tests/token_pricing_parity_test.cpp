// Golden parity test for the native token-pricing cluster.
//
// Replays every scenario under core/tools/golden/token-pricing/scenarios through
// agora::core::token_pricing and requires the produced trace to equal the frozen Kotlin trace byte
// for byte. The trace format mirrors the reference driver
// (core/tools/golden/token-pricing/TokenPricingTraceDump.kt).
//
// The pins that matter: the heuristic's ASCII-run ceiling and JVM whitespace boundary (NBSP is a
// token), the local counter's empty/negative/exception fallbacks, the byte fallback bounds, the
// Anthropic downscale bisection and tier threshold, Gemini's crop-unit truncation, the OpenAI
// patch/tile table including the shrink correction, the model-id regexes (release dates are not
// versions), the envelope table's largest-published fallback, and the safety margin's
// round-up-then-clamp.
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
#include "agora/core/token_pricing.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace tp = agora::core::token_pricing;
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

std::string opt(const std::optional<std::string>& value) {
    return value.has_value() ? esc(*value) : "-";
}

std::string opt(const std::optional<int>& value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

std::string opt(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

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

tp::ImageDescriptor read_image(const json::Value& root) {
    const json::Value* image = field(root, "image");
    if (image == nullptr || image->kind != Kind::Object) {
        scenario_fail("image must be an object");
    }
    tp::ImageDescriptor descriptor;
    if (const auto bytes = number(*image, "byteSize")) descriptor.byte_size = *bytes;
    if (const auto width = number(*image, "pixelWidth")) {
        descriptor.pixel_width = static_cast<int>(*width);
    }
    if (const auto height = number(*image, "pixelHeight")) {
        descriptor.pixel_height = static_cast<int>(*height);
    }
    return descriptor;
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

    if (op == "heuristic") {
        const std::string text = required_text(root, "text");
        trace += "args|" + esc(text) + "\n";
        trace += "result|" + std::to_string(tp::heuristic_text_tokens(text)) + "\n";
    } else if (op == "local") {
        const std::string text = required_text(root, "text");
        std::optional<int> exact;
        if (const auto value = number(root, "exact")) exact = static_cast<int>(*value);
        const bool throws = flag(root, "throws");
        trace += "args|" + esc(text) + "|" + opt(exact) + "|" + bool_text(throws) + "\n";
        trace += "result|" + std::to_string(tp::local_model_text_tokens(text, exact, throws)) +
                 "\n";
    } else if (op == "byteImage") {
        const tp::ImageDescriptor image = read_image(root);
        trace += "args|" + opt(image.byte_size) + "|" + opt(image.pixel_width) + "|" +
                 opt(image.pixel_height) + "\n";
        trace += "result|" + std::to_string(tp::byte_proportional_tokens(image)) + "\n";
    } else if (op == "anthropicTier") {
        const std::string model = required_text(root, "model");
        trace += "args|" + esc(model) + "\n";
        trace += "result|" + tp::describe(tp::anthropic_tier_for_model_name(model)) + "\n";
    } else if (op == "anthropicImage") {
        const std::string model = required_text(root, "model");
        const tp::ImageDescriptor image = read_image(root);
        trace += "args|" + esc(model) + "|" + esc(tp::describe(image)) + "\n";
        trace += "result|" +
                 std::to_string(tp::anthropic_image_tokens(
                     tp::anthropic_tier_for_model_name(model), image)) +
                 "\n";
    } else if (op == "geminiImage") {
        const tp::ImageDescriptor image = read_image(root);
        trace += "args|" + esc(tp::describe(image)) + "\n";
        trace += "result|" + std::to_string(tp::gemini_image_tokens(image)) + "\n";
    } else if (op == "openaiImage") {
        const std::string model = required_text(root, "model");
        const tp::ImageDescriptor image = read_image(root);
        trace += "args|" + esc(model) + "|" + esc(tp::describe(image)) + "\n";
        trace += "result|" + std::to_string(tp::openai_image_tokens(model, image)) + "\n";
    } else if (op == "anthropicModelId") {
        const std::string model = required_text(root, "model");
        trace += "args|" + esc(model) + "\n";
        trace += "result|" + esc(tp::describe(tp::AnthropicModelId::parse(model))) + "\n";
    } else if (op == "versionAtLeast") {
        const std::string model = required_text(root, "model");
        const int major = static_cast<int>(number(root, "major").value_or(0));
        const int minor = static_cast<int>(number(root, "minor").value_or(0));
        trace += "args|" + esc(model) + "|" + std::to_string(major) + "|" + std::to_string(minor) +
                 "\n";
        trace += "result|" +
                 bool_text(tp::AnthropicModelId::parse(model).version_at_least(major, minor)) + "\n";
    } else if (op == "envelopeDefault") {
        trace += "args|\n";
        trace += "result|" + esc(tp::describe(tp::default_envelope())) + "\n";
    } else if (op == "envelopeOpenAi") {
        trace += "args|\n";
        trace += "result|" + esc(tp::describe(tp::openai_envelope())) + "\n";
    } else if (op == "envelopeAnthropic") {
        const std::string model = required_text(root, "model");
        trace += "args|" + esc(model) + "\n";
        trace += "result|" + esc(tp::describe(tp::anthropic_envelope(model))) + "\n";
    } else if (op == "margin") {
        const std::int64_t raw = number(root, "raw").value_or(0);
        trace += "args|" + std::to_string(raw) + "\n";
        trace += "result|" + std::to_string(tp::default_safety_margin().apply(raw)) + "\n";
    } else if (op == "defaultModel") {
        const std::string text = required_text(root, "text");
        const tp::ImageDescriptor image = read_image(root);
        const std::int64_t raw = number(root, "raw").value_or(0);
        trace += "args|" + esc(text) + "|" + esc(tp::describe(image)) + "|" +
                 std::to_string(raw) + "\n";
        const tp::ContextCostModel model = tp::default_context_cost_model();
        trace += "result|" + std::to_string(model.text(text)) + "|" +
                 std::to_string(model.image(image)) + "|" +
                 esc(tp::describe(model.envelope)) + "|" +
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
