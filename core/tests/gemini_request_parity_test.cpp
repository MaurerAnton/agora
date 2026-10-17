// Golden parity test for the C++ Gemini request pipeline.
//
// Replays every scenario under core/tools/golden/gemini-request/scenarios through the C++ port
// (build_gemini_request / require_valid_wire_format /
// decode_api_generate_content_request) and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format, escaping, UUID normalization and mode handling mirror
// the reference driver (core/tools/golden/gemini-request/GeminiRequestTraceDump.kt) exactly.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/gemini_request.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace gemini = agora::core::gemini;
namespace openai = agora::core::openai;
namespace prep = agora::core::prep;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** The driver's norm(): only dashed UUIDs collapse to <uuid> (placeholders are dashed). */
std::string normalize_uuids(std::string_view value) {
    auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    std::string out;
    std::size_t index = 0;
    while (index < value.size()) {
        if (index + 36 <= value.size()) {
            bool ok = true;
            for (std::size_t offset = 0; offset < 36 && ok; ++offset) {
                const char c = value[index + offset];
                if (offset == 8 || offset == 13 || offset == 18 || offset == 23) {
                    ok = c == '-';
                } else {
                    ok = is_hex(c);
                }
            }
            if (ok) {
                out += "<uuid>";
                index += 36;
                continue;
            }
        }
        out.push_back(value[index]);
        ++index;
    }
    return out;
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

std::string hex_lower(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

// -- scenario decoding (the @Serializable Scenario shape of the reference driver) -----------------

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
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

bool bool_or(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal || (value->text != "true" && value->text != "false")) {
        scenario_fail(std::string(name), "expected a JSON boolean");
    }
    return value->text == "true";
}

std::optional<std::int32_t> optional_int(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::Literal || value->text.find_first_of(".eE") != std::string::npos) {
        scenario_fail(std::string(name), "expected an Int");
    }
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value->text.data(),
                                        value->text.data() + value->text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
        scenario_fail(std::string(name), "expected an Int");
    }
    return parsed;
}

float scenario_float(const json::Value& value) {
    if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) {
        scenario_fail("float", "expected a number or a special-value string");
    }
    const std::string& text = value.text;
    if (text == "NaN") return std::numeric_limits<float>::quiet_NaN();
    if (text == "Infinity") return std::numeric_limits<float>::infinity();
    if (text == "-Infinity") return -std::numeric_limits<float>::infinity();
    float parsed = 0.0f;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        scenario_fail("float", "cannot parse '" + text + "'");
    }
    return parsed;
}

std::optional<float> optional_scenario_float(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    return scenario_float(*value);
}

std::vector<std::string> string_list(const json::Value* value) {
    std::vector<std::string> out;
    if (value == nullptr || is_json_null(value)) return out;
    if (value->kind != json::Kind::Array) scenario_fail("list", "expected a JSON array");
    for (const json::Value& entry : value->elements) {
        if (entry.kind != json::Kind::String) scenario_fail("list", "expected strings");
        out.push_back(entry.text);
    }
    return out;
}

// -- tool building: the app's programmatic ToolDefinition construction --------------------------

std::optional<openai::ToolProperty> build_tool_property(const json::Value& value,
                                                        const std::string& name) {
    if (is_json_null(&value)) return std::nullopt;
    if (value.kind != json::Kind::Object) {
        scenario_fail("tool property " + name, "expected an object");
    }
    openai::ToolProperty property;
    const json::Value* type = field(value, "type");
    if (type == nullptr || type->kind != json::Kind::String) {
        scenario_fail("tool property " + name, "requires a type string");
    }
    property.type = type->text;
    const json::Value* description = field(value, "description");
    if (description == nullptr || description->kind != json::Kind::String) {
        scenario_fail("tool property " + name, "requires a description string");
    }
    property.description = description->text;
    if (const json::Value* items = field(value, "items"); items != nullptr && !is_json_null(items)) {
        auto child = build_tool_property(*items, name + "[]");
        if (!child) scenario_fail("tool property " + name + "[]", "must not be null");
        property.items = std::make_shared<openai::ToolProperty>(std::move(*child));
    }
    if (const json::Value* properties = field(value, "properties");
        properties != nullptr && !is_json_null(properties)) {
        if (properties->kind != json::Kind::Object) {
            scenario_fail("tool property " + name, "properties must be an object");
        }
        std::vector<std::pair<std::string, openai::ToolProperty>> nested;
        for (const json::Member& entry : properties->members) {
            auto child = build_tool_property(entry.value, entry.key);
            if (!child) scenario_fail("tool property " + entry.key, "must not be null");
            nested.emplace_back(entry.key, std::move(*child));
        }
        property.properties = std::move(nested);
    }
    if (const json::Value* required = field(value, "required");
        required != nullptr && !is_json_null(required)) {
        if (required->kind != json::Kind::Array) {
            scenario_fail("tool property " + name, "required must be an array");
        }
        std::vector<std::string> names;
        for (const json::Value& entry : required->elements) {
            if (entry.kind == json::Kind::String || entry.kind == json::Kind::Literal) {
                names.push_back(entry.text);
            } else {
                scenario_fail("tool property " + name, "required entries must be primitives");
            }
        }
        property.required = std::move(names);
    }
    return property;
}

openai::ToolParameters build_tool_parameters(const json::Value* value) {
    openai::ToolParameters parameters;
    if (value == nullptr || is_json_null(value)) return parameters;
    if (value->kind != json::Kind::Object) scenario_fail("tool parameters", "expected an object");
    if (const json::Value* type = field(*value, "type"); type != nullptr && !is_json_null(type)) {
        if (type->kind != json::Kind::String) scenario_fail("tool parameters", "type must be a string");
        parameters.type = type->text;
    }
    if (const json::Value* properties = field(*value, "properties");
        properties != nullptr && !is_json_null(properties)) {
        if (properties->kind != json::Kind::Object) {
            scenario_fail("tool parameters", "properties must be an object");
        }
        for (const json::Member& entry : properties->members) {
            auto property = build_tool_property(entry.value, entry.key);
            if (!property) scenario_fail("tool parameters", "property '" + entry.key + "' must not be null");
            parameters.properties.emplace_back(entry.key, std::move(*property));
        }
    }
    if (const json::Value* required = field(*value, "required");
        required != nullptr && !is_json_null(required)) {
        if (required->kind != json::Kind::Array) scenario_fail("tool parameters", "required must be an array");
        for (const json::Value& entry : required->elements) {
            if (entry.kind == json::Kind::String || entry.kind == json::Kind::Literal) {
                parameters.required.push_back(entry.text);
            } else {
                scenario_fail("tool parameters", "required entries must be primitives");
            }
        }
    }
    if (const json::Value* schema = field(*value, "schema");
        schema != nullptr && !is_json_null(schema)) {
        if (schema->kind != json::Kind::Object) scenario_fail("tool parameters", "schema must be an object");
        parameters.schema = *schema;
    }
    return parameters;
}

std::optional<std::vector<openai::ToolDefinition>> build_tools(const json::Value* raw_tools) {
    if (raw_tools == nullptr || is_json_null(raw_tools)) return std::nullopt;
    if (raw_tools->kind != json::Kind::Array) scenario_fail("tools", "expected an array");
    std::vector<openai::ToolDefinition> tools;
    for (const json::Value& raw : raw_tools->elements) {
        if (raw.kind != json::Kind::Object) scenario_fail("tools", "expected objects");
        const json::Value* function = field(raw, "function");
        if (function == nullptr || function->kind != json::Kind::Object) {
            scenario_fail("tools", "tool without function");
        }
        openai::ToolDefinition tool;
        if (const json::Value* type = field(raw, "type"); type != nullptr && !is_json_null(type)) {
            if (type->kind != json::Kind::String) scenario_fail("tools", "type must be a string");
            tool.type = type->text;
        }
        const json::Value* name = field(*function, "name");
        if (name == nullptr || name->kind != json::Kind::String) {
            scenario_fail("tools", "tool without name");
        }
        tool.function.name = name->text;
        const json::Value* description = field(*function, "description");
        if (description != nullptr && !is_json_null(description)) {
            if (description->kind != json::Kind::String) {
                scenario_fail("tools", "description must be a string");
            }
            tool.function.description = description->text;
        }
        tool.function.parameters = build_tool_parameters(field(*function, "parameters"));
        tools.push_back(std::move(tool));
    }
    return tools;
}

// -- conversation input --------------------------------------------------------------------------

prep::Participant parse_participant(const std::string& value) {
    if (value == "USER") return prep::Participant::User;
    if (value == "MODEL") return prep::Participant::Model;
    if (value == "ERROR") return prep::Participant::Error;
    scenario_fail("participant", "unknown participant: " + value);
}

prep::ChatMessage build_message(const std::filesystem::path& files_root,
                                const json::Value& message, const std::string& scenario_name) {
    prep::ChatMessage built;
    built.id = string_or(message, "id", "");
    built.text = string_or(message, "text", "");
    built.participant = parse_participant(string_or(message, "participant", "USER"));
    built.model_name = optional_string(message, "modelName");

    for (const std::string& reference : string_list(field(message, "images"))) {
        std::error_code error;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(files_root / reference, error);
        built.images.push_back((error ? files_root / reference : canonical).string());
    }

    if (const json::Value* segments = field(message, "segments");
        segments != nullptr && !is_json_null(segments)) {
        if (segments->kind != json::Kind::Array) scenario_fail(scenario_name, "segments must be an array");
        std::vector<prep::MessageSegment> decoded;
        for (const json::Value& entry : segments->elements) {
            if (entry.kind != json::Kind::Object) scenario_fail(scenario_name, "segment must be an object");
            prep::MessageSegment segment;
            segment.type = string_or(entry, "type", "");
            segment.content = string_or(entry, "content", "");
            segment.tool_name = optional_string(entry, "toolName");
            segment.tool_args = optional_string(entry, "toolArgs");
            segment.tool_result = optional_string(entry, "toolResult");
            segment.tool_call_id = optional_string(entry, "toolCallId");
            segment.signature = optional_string(entry, "signature");
            segment.signature_provider = optional_string(entry, "signatureProvider");
            if (const json::Value* items = field(entry, "responseOutputItems");
                items != nullptr && !is_json_null(items)) {
                if (items->kind != json::Kind::Array) scenario_fail(scenario_name, "items must be an array");
                segment.response_output_items = items->elements;
            }
            segment.response_output_item_provider =
                optional_string(entry, "responseOutputItemProvider");
            decoded.push_back(std::move(segment));
        }
        built.segments = std::move(decoded);
    }

    if (const json::Value* call = field(message, "toolCall");
        call != nullptr && !is_json_null(call)) {
        if (call->kind != json::Kind::Object) scenario_fail(scenario_name, "toolCall must be an object");
        prep::ToolCallData data;
        data.tool_name = string_or(*call, "toolName", "");
        data.arguments = string_or(*call, "arguments", "");
        data.result = string_or(*call, "result", "");
        data.signature = optional_string(*call, "signature");
        data.tool_call_id = optional_string(*call, "toolCallId");
        if (const json::Value* items = field(*call, "responseOutputItems");
            items != nullptr && !is_json_null(items)) {
            if (items->kind != json::Kind::Array) scenario_fail(scenario_name, "items must be an array");
            data.response_output_items = items->elements;
        }
        data.response_output_item_provider = optional_string(*call, "responseOutputItemProvider");
        built.tool_call = std::move(data);
    }
    return built;
}

openai::ProviderConfig build_config(const json::Value& config) {
    openai::ProviderConfig built;
    built.model_id = string_or(config, "modelId", "gemini-2.5-pro");
    built.thinking_enabled = bool_or(config, "thinkingEnabled", true);
    built.thinking_level = string_or(config, "thinkingLevel", "medium");
    built.thinking_budget_enabled = bool_or(config, "thinkingBudgetEnabled", false);
    built.thinking_budget_tokens = optional_int(config, "thinkingBudgetTokens").value_or(4096);
    built.code_execution_enabled = bool_or(config, "codeExecutionEnabled", false);
    built.google_search_enabled = bool_or(config, "googleSearchEnabled", false);
    built.include_images = bool_or(config, "includeImages", true);
    built.temperature = optional_scenario_float(config, "temperature");
    built.max_tokens = optional_int(config, "maxTokens");
    built.top_p = optional_scenario_float(config, "topP");
    built.frequency_penalty = optional_scenario_float(config, "frequencyPenalty");
    built.presence_penalty = optional_scenario_float(config, "presencePenalty");
    built.tools = build_tools(field(config, "tools"));
    return built;
}

std::string rel_path(const std::filesystem::path& files_root, const std::string& file) {
    std::error_code error;
    const std::filesystem::path relative =
        std::filesystem::relative(std::filesystem::path(file), files_root, error);
    if (error) return file;
    return relative.string();
}

// -- scenario runner ------------------------------------------------------------------------------

std::string run_scenario(const std::filesystem::path& scenario_path,
                         const std::filesystem::path& files_root) {
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
    const std::string provider_name = string_or(scenario, "providerName", "Google");
    const std::string mode = string_or(scenario, "mode", "factory");

    std::string out;
    const auto line = [&](const std::string& first, const std::string& second) {
        out += first;
        out += '|';
        out += second;
        out += '\n';
    };
    line("name", esc(name));
    line("mode", esc(mode));

    try {
        if (mode == "serialized") {
            const auto body = optional_string(scenario, "serializedBody");
            if (!body) scenario_fail(stem, "serialized mode requires serializedBody");
            openai::require_valid_serialized_request(
                provider_name, *body,
                string_list(field(scenario, "serializedRequiredStringFields")),
                string_list(field(scenario, "serializedRequiredArrayFields")));
            out += "ok\n";
        } else if (mode == "validate") {
            const json::Value* request_json = field(scenario, "request");
            if (request_json == nullptr) scenario_fail(stem, "validate mode requires request");
            const json::Value empty;
            const json::Value* config_pointer = field(scenario, "config");
            const json::Value& config_json = config_pointer ? *config_pointer : empty;
            const openai::ProviderConfig config = build_config(config_json);
            std::string clean_model = config.model_id;
            constexpr std::string_view kPrefix = "models/";
            if (clean_model.rfind(kPrefix, 0) == 0) clean_model.erase(0, kPrefix.size());
            const gemini::ApiGenerateContentRequest decoded =
                gemini::decode_api_generate_content_request(*request_json);
            gemini::require_valid_wire_format(decoded, clean_model);
            out += "ok\n";
        } else {
            const json::Value empty;
            const json::Value* config_pointer = field(scenario, "config");
            const json::Value& config_json = config_pointer ? *config_pointer : empty;
            openai::ProviderConfig config = build_config(config_json);

            std::vector<prep::ChatMessage> messages;
            std::optional<std::string> system_prompt;
            if (const json::Value* input_json = field(scenario, "requestInput");
                input_json != nullptr && !is_json_null(input_json)) {
                if (input_json->kind != json::Kind::Object) {
                    scenario_fail(stem, "requestInput must be an object");
                }
                system_prompt = optional_string(*input_json, "systemPrompt");
                if (const json::Value* messages_json = field(*input_json, "messages");
                    messages_json != nullptr && !is_json_null(messages_json)) {
                    if (messages_json->kind != json::Kind::Array) {
                        scenario_fail(stem, "messages must be an array");
                    }
                    for (const json::Value& entry : messages_json->elements) {
                        messages.push_back(build_message(files_root, entry, stem));
                    }
                }
            }

            const gemini::BuiltGeminiRequest built =
                gemini::build_gemini_request(provider_name, config, messages, system_prompt);
            line("contents", normalize_uuids(esc(gemini::write_api_request_content_list(
                                 built.contents))));
            line("body", normalize_uuids(esc(built.body_json)));
            if (built.streaming_body) {
                const openai::StreamingJsonBody& body = *built.streaming_body;
                line("diagnostic", normalize_uuids(esc(body.diagnostic_json)));
                line("contentLength", std::to_string(body.content_length));
                for (std::size_t index = 0; index < body.parts.size(); ++index) {
                    const openai::Base64FilePart& part = body.parts[index];
                    out += "file|" + std::to_string(index) + "|" +
                           esc(rel_path(files_root, part.path)) + "|" +
                           std::to_string(part.byte_count) + "|" + hex_lower(part.sha256) + "\n";
                }
                const std::string resolved = body.resolved_bytes();
                if (static_cast<std::int64_t>(resolved.size()) != body.content_length) {
                    throw gemini::ProviderRequestError{
                        "IllegalStateException",
                        "resolved body is " + std::to_string(resolved.size()) +
                            " bytes but contentLength is " +
                            std::to_string(body.content_length)};
                }
                line("resolvedBody", normalize_uuids(esc(resolved)));
            }
        }
    } catch (const gemini::ProviderRequestError& error) {
        // Only the request-format gate carries a portable, semantic message.
        out += "error|" + error.class_name + "|";
        if (error.class_name == "RequestFormatException") out += esc(error.message);
        out += '\n';
    } catch (const agora::core::test::Failure&) {
        throw;
    } catch (const std::exception& error) {
        out += "error|";
        out += error.what();
        out += "|\n";
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

TEST(gemini_request_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_GEMINI_REQUEST_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(root / "files"));

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
        const std::string actual = run_scenario(scenario, root);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        checked++;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
