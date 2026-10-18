// Golden parity test for the C++ MCP protocol port.
//
// Replays every scenario under core/tools/golden/mcp-protocol/scenarios through the C++ port
// (public_mcp_tool_name / project_tool_definition / parse_call_payload / parse_tools_page /
// validate_rpc_response / protocol_candidates / negotiated_protocol_version) and requires the
// produced trace to equal the frozen Kotlin trace byte for byte. The trace format and mode
// handling mirror the reference driver (core/tools/golden/mcp-protocol/McpProtocolTraceDump.kt).
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

#include "agora/core/json.hpp"
#include "agora/core/mcp_protocol.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace mcp = agora::core::mcp;
namespace openai = agora::core::openai;

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

std::string or_null(const std::optional<std::string>& value) {
    return value ? esc(*value) : "-";
}

// -- scenario decoding -----------------------------------------------------------------------------

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

std::int64_t int64_or(const json::Value& object, std::string_view name, std::int64_t fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected an Int");
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value->text.data(),
                                        value->text.data() + value->text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
        scenario_fail(std::string(name), "expected an Int");
    }
    return parsed;
}

const json::Value* object_or_empty(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Object) return nullptr;
    return value;
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
    const std::string mode = string_or(scenario, "mode", "toolName");
    const std::string server_id = string_or(scenario, "serverId", "srv-1");
    const std::string server_name = string_or(scenario, "serverName", "Server One");
    const std::string remote_name = string_or(scenario, "remoteName", "search");

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

    try {
        if (mode == "toolName") {
            line({"name", esc(mcp::public_mcp_tool_name(server_id, remote_name))});
        } else if (mode == "toolDefinition") {
            mcp::RemoteTool remote;
            remote.name = remote_name;
            remote.description = string_or(scenario, "description", "");
            if (const json::Value* schema = object_or_empty(scenario, "inputSchema")) {
                remote.input_schema = *schema;
            } else {
                json::Value fallback;
                fallback.kind = json::Kind::Object;
                json::Value type;
                type.kind = json::Kind::String;
                type.text = "object";
                json::Value properties;
                properties.kind = json::Kind::Object;
                fallback.members.push_back({"type", type});
                fallback.members.push_back({"properties", properties});
                remote.input_schema = std::move(fallback);
            }
            const mcp::ToolDefinitionProjection projection = mcp::project_tool_definition(
                mcp::public_mcp_tool_name(server_id, remote_name), server_name, remote);
            std::string definition_json;
            openai::write_tool_definition(definition_json, projection.definition);
            line({"definition", esc(definition_json)});
            line({"schema", esc(json::to_kotlin_string(projection.schema))});
        } else if (mode == "callPayload") {
            const json::Value result = object_or_empty(scenario, "result")
                                           ? *object_or_empty(scenario, "result")
                                           : json::Value();
            const mcp::CallPayload payload = mcp::parse_call_payload(result);
            for (std::size_t i = 0; i < payload.text_parts.size(); ++i) {
                line({"text", std::to_string(i), esc(payload.text_parts[i])});
            }
            for (std::size_t i = 0; i < payload.images.size(); ++i) {
                line({"image", std::to_string(i), esc(payload.images[i].data),
                      esc(payload.images[i].mime_type)});
            }
            line({"structured", payload.structured_content
                                    ? esc(json::to_kotlin_string(*payload.structured_content))
                                    : "-"});
            line({"isError", payload.is_error ? "1" : "0"});
        } else if (mode == "toolsPage") {
            const json::Value result = object_or_empty(scenario, "result")
                                           ? *object_or_empty(scenario, "result")
                                           : json::Value();
            const mcp::ToolsPage page = mcp::parse_tools_page(result);
            for (std::size_t i = 0; i < page.tools.size(); ++i) {
                line({"tool", std::to_string(i), esc(page.tools[i].name),
                      esc(page.tools[i].description),
                      esc(json::to_kotlin_string(page.tools[i].input_schema))});
            }
            line({"cursor", or_null(page.cursor)});
        } else if (mode == "rpcResponse") {
            const std::string method = string_or(scenario, "method", "tools/call");
            const std::int64_t id = int64_or(scenario, "id", 1);
            const json::Value* response = object_or_empty(scenario, "response");
            const std::optional<json::Value> response_value =
                response ? std::optional<json::Value>(*response) : std::nullopt;
            const json::Value result = mcp::validate_rpc_response(method, id, response_value);
            line({"result", esc(json::to_kotlin_string(result))});
        } else if (mode == "negotiation") {
            const std::string transport = string_or(scenario, "transportType", "STREAMABLE_HTTP");
            const std::string current = string_or(scenario, "currentVersion", "2025-11-25");
            const std::vector<std::string> candidates =
                mcp::protocol_candidates(transport == "STREAMABLE_HTTP", current);
            std::string joined;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (i > 0) joined.push_back(',');
                joined += candidates[i];
            }
            line({"candidates", joined});
        } else if (mode == "unsupportedVersion") {
            line({"unsupported",
                  mcp::is_unsupported_protocol_version(string_list_or(scenario, "messages")) ? "1"
                                                                                            : "0"});
        } else if (mode == "initialize") {
            const json::Value result = object_or_empty(scenario, "result")
                                           ? *object_or_empty(scenario, "result")
                                           : json::Value();
            const std::string negotiated = mcp::negotiated_protocol_version(result);
            line({"negotiated", esc(negotiated)});
            line({"sameAsRequested",
                  negotiated == string_or(scenario, "currentVersion", "2025-11-25") ? "1" : "0"});
        } else {
            scenario_fail(stem, "unknown mode: " + mode);
        }
    } catch (const openai::ProviderRequestError& error) {
        line({"error", error.class_name, esc(error.message)});
    } catch (const agora::core::test::Failure&) {
        throw;
    } catch (const std::exception& error) {
        line({"error", error.what(), ""});
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

TEST(mcp_protocol_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MCP_PROTOCOL_DIR;
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