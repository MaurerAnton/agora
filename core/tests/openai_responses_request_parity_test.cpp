// Golden parity test for the OpenAI Responses request builder and wire-format gate.
//
// The scenarios feed the port the same chat messages / tool definitions / request the Kotlin
// importer sees; the trace pins the produced input items, the converted tools and every violation
// the local gate reports (with the JDK-style message and the distinct violation list).
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/openai_responses_request.hpp"
#include "check.hpp"

namespace {

namespace responses = agora::core::openai_responses;
namespace openai = agora::core::openai;
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

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::string();
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::optional<std::string> maybe_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::optional<int> maybe_int(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a number");
    return std::stoi(value->text);
}

std::optional<float> maybe_float(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a number");
    return std::stof(value->text);
}

// ------------------------------------------------------------------ scenario decoding

openai::OpenAiMessage build_message(const Value& spec) {
    openai::OpenAiMessage message;
    message.role = optional_string(spec, "role");
    if (const Value* content = field(spec, "content"); content != nullptr) {
        std::vector<openai::OpenAiContentPart> parts;
        for (const Value& element : content->elements) {
            openai::OpenAiContentPart part;
            part.type = optional_string(element, "type");
            part.text = maybe_string(element, "text");
            if (const Value* image = field(element, "image_url"); image != nullptr) {
                openai::OpenAiImageUrl url;
                url.url = optional_string(*image, "url");
                part.image_url = url;
            }
            parts.push_back(std::move(part));
        }
        message.content = std::move(parts);
    }
    if (const Value* calls = field(spec, "tool_calls"); calls != nullptr) {
        std::vector<openai::OpenAiRequestToolCall> tool_calls;
        for (const Value& element : calls->elements) {
            openai::OpenAiRequestToolCall call;
            call.id = optional_string(element, "id");
            call.type = maybe_string(element, "type").value_or("function");
            const Value* function = field(element, "function");
            if (function == nullptr) scenario_fail("tool call needs a function");
            call.function.name = optional_string(*function, "name");
            call.function.arguments = maybe_string(*function, "arguments").value_or("");
            tool_calls.push_back(std::move(call));
        }
        message.tool_calls = std::move(tool_calls);
    }
    message.tool_call_id = maybe_string(spec, "tool_call_id");
    message.reasoning_content = maybe_string(spec, "reasoning_content");
    if (const Value* items = field(spec, "response_output_items"); items != nullptr) {
        std::vector<Value> output_items;
        for (const Value& element : items->elements) output_items.push_back(element);
        message.response_output_items = std::move(output_items);
    }
    message.response_output_item_provider = maybe_string(spec, "response_output_item_provider");
    message.response_output_item_model = maybe_string(spec, "response_output_item_model");
    return message;
}

openai::ToolProperty build_property(const Value& value) {
    openai::ToolProperty property;
    property.type = optional_string(value, "type");
    property.description = optional_string(value, "description");
    if (const Value* items = field(value, "items"); items != nullptr) {
        property.items = std::make_shared<openai::ToolProperty>(build_property(*items));
    }
    if (const Value* properties = field(value, "properties"); properties != nullptr) {
        std::vector<std::pair<std::string, openai::ToolProperty>> entries;
        for (const Member& member : properties->members) {
            entries.emplace_back(member.key, build_property(member.value));
        }
        property.properties = std::move(entries);
    }
    if (const Value* required = field(value, "required"); required != nullptr) {
        std::vector<std::string> names;
        for (const Value& element : required->elements) names.push_back(element.text);
        property.required = std::move(names);
    }
    return property;
}

openai::ToolParameters build_parameters(const Value& value) {
    openai::ToolParameters parameters;
    parameters.type = maybe_string(value, "type").value_or("object");
    if (const Value* properties = field(value, "properties"); properties != nullptr) {
        std::vector<std::pair<std::string, openai::ToolProperty>> entries;
        for (const Member& member : properties->members) {
            entries.emplace_back(member.key, build_property(member.value));
        }
        parameters.properties = std::move(entries);
    }
    if (const Value* required = field(value, "required"); required != nullptr) {
        std::vector<std::string> names;
        for (const Value& element : required->elements) names.push_back(element.text);
        parameters.required = std::move(names);
    }
    if (const Value* schema = field(value, "schema"); schema != nullptr) {
        parameters.schema = *schema;
    }
    return parameters;
}

openai::ToolDefinition build_tool(const Value& value) {
    openai::ToolDefinition tool;
    tool.type = maybe_string(value, "type").value_or("function");
    const Value* function = field(value, "function");
    if (function != nullptr) {
        tool.function.name = optional_string(*function, "name");
        tool.function.description = optional_string(*function, "description");
        if (const Value* parameters = field(*function, "parameters"); parameters != nullptr) {
            tool.function.parameters = build_parameters(*parameters);
        }
    }
    return tool;
}

responses::ResponseTool build_response_tool(const Value& value) {
    responses::ResponseTool tool;
    tool.type = maybe_string(value, "type").value_or("function");
    tool.name = maybe_string(value, "name");
    tool.description = maybe_string(value, "description");
    if (const Value* parameters = field(value, "parameters"); parameters != nullptr) {
        tool.parameters = build_parameters(*parameters);
    }
    return tool;
}

responses::ResponsesRequest build_request(const Value& value) {
    responses::ResponsesRequest request;
    request.model = optional_string(value, "model");
    if (const Value* input = field(value, "input"); input != nullptr) {
        for (const Value& element : input->elements) request.input.push_back(element);
    }
    if (const Value* stream = field(value, "stream"); stream != nullptr) {
        request.stream = stream->kind == Kind::Literal && stream->text == "true";
    }
    if (const Value* tools = field(value, "tools"); tools != nullptr) {
        std::vector<responses::ResponseTool> converted;
        for (const Value& element : tools->elements) converted.push_back(build_response_tool(element));
        request.tools = std::move(converted);
    }
    if (const Value* reasoning = field(value, "reasoning"); reasoning != nullptr) {
        openai::OpenAiReasoning parsed;
        parsed.effort = maybe_string(*reasoning, "effort");
        parsed.summary = maybe_string(*reasoning, "summary");
        parsed.max_tokens = maybe_int(*reasoning, "max_tokens");
        if (const Value* enabled = field(*reasoning, "enabled"); enabled != nullptr) {
            parsed.enabled = enabled->kind == Kind::Literal && enabled->text == "true";
        }
        request.reasoning = parsed;
    }
    request.service_tier = maybe_string(value, "service_tier");
    request.temperature = maybe_float(value, "temperature");
    request.max_output_tokens = maybe_int(value, "max_output_tokens");
    request.top_p = maybe_float(value, "top_p");
    request.prompt_cache_key = maybe_string(value, "prompt_cache_key");
    return request;
}

// -------------------------------------------------------------------- tool rendering

std::string render_property(const openai::ToolProperty& property) {
    std::string out = "{\"type\":";
    append_quoted(out, property.type);
    out += ",\"description\":";
    append_quoted(out, property.description);
    if (property.items) {
        out += ",\"items\":" + render_property(*property.items);
    }
    if (property.properties.has_value()) {
        out += ",\"properties\":{";
        bool first = true;
        for (const auto& entry : *property.properties) {
            if (!first) out.push_back(',');
            first = false;
            append_quoted(out, entry.first);
            out += ":" + render_property(entry.second);
        }
        out.push_back('}');
    }
    if (property.required.has_value()) {
        out += ",\"required\":[";
        for (std::size_t i = 0; i < property.required->size(); ++i) {
            if (i != 0) out.push_back(',');
            append_quoted(out, (*property.required)[i]);
        }
        out.push_back(']');
    }
    out.push_back('}');
    return out;
}

std::string render_parameters(const openai::ToolParameters& parameters) {
    if (parameters.schema.has_value()) return json::to_kotlin_string(*parameters.schema);
    std::string out = "{\"type\":";
    append_quoted(out, parameters.type);
    out += ",\"properties\":{";
    bool first = true;
    for (const auto& entry : parameters.properties) {
        if (!first) out.push_back(',');
        first = false;
        append_quoted(out, entry.first);
        out += ":" + render_property(entry.second);
    }
    out += "},\"required\":[";
    for (std::size_t i = 0; i < parameters.required.size(); ++i) {
        if (i != 0) out.push_back(',');
        append_quoted(out, parameters.required[i]);
    }
    out += "]}";
    return out;
}

std::string render_tools(const std::vector<responses::ResponseTool>& tools) {
    std::string out = "[";
    for (std::size_t i = 0; i < tools.size(); ++i) {
        if (i != 0) out.push_back(',');
        const responses::ResponseTool& tool = tools[i];
        out += "{\"type\":";
        append_quoted(out, tool.type);
        if (tool.name.has_value()) {
            out += ",\"name\":";
            append_quoted(out, *tool.name);
        }
        if (tool.description.has_value()) {
            out += ",\"description\":";
            append_quoted(out, *tool.description);
        }
        if (tool.parameters.has_value()) {
            out += ",\"parameters\":" + render_parameters(*tool.parameters);
        }
        out.push_back('}');
    }
    out.push_back(']');
    return out;
}

// ------------------------------------------------------------------------- the scenario

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const std::string op = optional_string(root, "op");

    if (op == "input") {
        std::vector<openai::OpenAiMessage> messages;
        if (const Value* list = field(root, "messages"); list != nullptr) {
            for (const Value& element : list->elements) {
                messages.push_back(build_message(element));
            }
        }
        const std::optional<std::string> provider = maybe_string(root, "provider");
        const std::optional<std::string> target_model = maybe_string(root, "target_model");
        const std::vector<Value> input =
            responses::to_responses_input(messages, provider, target_model);
        Value array;
        array.kind = Kind::Array;
        array.elements = input;
        return "{\"input\":" + json::to_kotlin_string(array) + "}";
    }

    if (op == "tools") {
        std::vector<openai::ToolDefinition> tools;
        if (const Value* list = field(root, "tools"); list != nullptr) {
            for (const Value& element : list->elements) tools.push_back(build_tool(element));
        }
        return "{\"tools\":" + render_tools(responses::to_responses_tools(tools)) + "}";
    }

    if (op == "validate") {
        const Value* request_value = field(root, "request");
        if (request_value == nullptr) scenario_fail("`request` is required");
        const responses::ResponsesRequest request = build_request(*request_value);
        const std::string provider = maybe_string(root, "provider").value_or("openai");
        try {
            responses::require_valid_wire_format(request, provider);
            return "{\"ok\":true}";
        } catch (const responses::RequestFormatException& error) {
            std::string out = "{\"error\":{\"type\":\"RequestFormatException\",\"message\":";
            append_quoted(out, error.what());
            out += ",\"violations\":[";
            for (std::size_t i = 0; i < error.violations().size(); ++i) {
                if (i != 0) out.push_back(',');
                append_quoted(out, error.violations()[i]);
            }
            out += "]}}";
            return out;
        }
    }

    scenario_fail(path.stem().string() + ": unknown op " + op);
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(openai_responses_request_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_OPENAI_RESPONSES_REQUEST_DIR;
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
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
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
