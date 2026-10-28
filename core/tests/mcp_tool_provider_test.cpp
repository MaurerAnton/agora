// Unit tests for the McpToolProvider port: registry-driven definitions, descriptor-gated
// handles/presentation metadata and the TargetResolved → Progress → Completed event sequence.

#include "agora/core/mcp_tool_provider.hpp"

#include <optional>
#include <string>
#include <vector>

#include "agora/core/json.hpp"
#include "check.hpp"

namespace mcp = agora::core::mcp;
namespace shell_provider = agora::core::shell_provider;

namespace {

namespace json = agora::core::json;

json::Value parse_json(const std::string& text) {
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.value) throw std::runtime_error("bad schema json");
    return *parsed.value;
}

struct ScriptedRegistry final : mcp::Registry {
    std::vector<ToolInfo> tools;
    shell_provider::ToolExecutionResult result;
    std::string last_name;
    std::string last_args;
    int execute_calls = 0;

    ScriptedRegistry() { result.text = "mcp-result"; }

    std::vector<ToolInfo> enabled_tools() override { return tools; }

    std::optional<ToolInfo> descriptor(const std::string& public_name) override {
        for (const ToolInfo& tool : tools) {
            if (tool.public_name == public_name) return tool;
        }
        return std::nullopt;
    }

    shell_provider::ToolExecutionResult execute(const std::string& public_name,
                                                const std::string& arguments) override {
        ++execute_calls;
        last_name = public_name;
        last_args = arguments;
        return result;
    }
};

mcp::Registry::ToolInfo make_tool(std::string public_name, std::string server_name,
                                  std::string remote_name, std::string description) {
    mcp::Registry::ToolInfo tool;
    tool.public_name = std::move(public_name);
    tool.server_name = std::move(server_name);
    tool.remote.name = std::move(remote_name);
    tool.remote.description = std::move(description);
    tool.remote.input_schema = parse_json(
        R"({"type":"object","properties":{"q":{"type":"string"}}})");
    return tool;
}

struct Fixture {
    ScriptedRegistry registry;
    mcp::McpToolProvider provider;

    Fixture() : provider(registry) {}
};

}  // namespace

TEST(mcp_definitions_come_from_the_registry) {
    Fixture fixture;
    const auto ctx = shell_provider::GenerationContext{};
    CHECK(fixture.provider.definitions(ctx).empty());

    fixture.registry.tools.push_back(
        make_tool("mcp_srv_tool_abc", "srv", "tool", "Does the tool thing."));
    fixture.registry.tools.push_back(
        make_tool("mcp_srv_other_xyz", "srv", "other", ""));  // blank description falls back

    const auto definitions = fixture.provider.definitions(ctx);
    CHECK_EQ(std::size_t(2), definitions.size());

    // asToolDefinition (via the pinned project_tool_definition): name, description + banner.
    CHECK_EQ(std::string("mcp_srv_tool_abc"), definitions[0].function.name);
    CHECK_EQ(std::string("Does the tool thing.\n\nProvided by MCP server: srv"),
             definitions[0].function.description);
    CHECK_EQ(std::string("mcp_srv_other_xyz"), definitions[1].function.name);
    CHECK_EQ(std::string("MCP tool other\n\nProvided by MCP server: srv"),
             definitions[1].function.description);

    // The remote inputSchema travels as the external parameters schema.
    CHECK(definitions[0].function.parameters.schema.has_value());
    CHECK_EQ(std::string("q"), definitions[0].function.parameters.properties.front().first);
}

TEST(mcp_handles_follow_descriptors) {
    Fixture fixture;
    fixture.registry.tools.push_back(make_tool("mcp_a", "srv", "a", "d"));
    CHECK(fixture.provider.handles("mcp_a"));
    CHECK(!fixture.provider.handles("mcp_missing"));
    CHECK(!fixture.provider.handles("web_search"));
}

TEST(mcp_presentation_metadata_maps_descriptor) {
    Fixture fixture;
    const auto ctx = shell_provider::GenerationContext{};
    CHECK(!fixture.provider.presentation_metadata("mcp_a").has_value());

    fixture.registry.tools.push_back(make_tool("mcp_a", "srv", "remote_tool_name", "d"));
    const auto metadata = fixture.provider.presentation_metadata("mcp_a");
    CHECK(metadata.has_value());
    CHECK_EQ(std::string("remote_tool_name"), metadata->display_name);
    CHECK(metadata->target.has_value());
    CHECK_EQ(std::string("srv"), *metadata->target);
}

TEST(mcp_execute_forwards_to_the_registry) {
    Fixture fixture;
    const auto ctx = shell_provider::GenerationContext{};
    fixture.registry.result.text = R"({"type":"tool","ok":true})";
    const std::string text =
        fixture.provider.execute("mcp_a", R"({"q":"x"})", ctx);
    CHECK_EQ(std::string(R"({"type":"tool","ok":true})"), text);
    CHECK_EQ(1, fixture.registry.execute_calls);
    CHECK_EQ(std::string("mcp_a"), fixture.registry.last_name);
    CHECK_EQ(std::string(R"({"q":"x"})"), fixture.registry.last_args);
}

TEST(mcp_execute_events_emits_target_progress_completed) {
    Fixture fixture;
    const auto ctx = shell_provider::GenerationContext{};
    fixture.registry.tools.push_back(make_tool("mcp_a", "srv", "a", "d"));
    fixture.registry.result.text = "done";
    fixture.registry.result.is_error = false;

    std::vector<shell_provider::ToolExecutionEvent> events;
    const auto sink = [&](const shell_provider::ToolExecutionEvent& event) {
        events.push_back(event);
    };
    fixture.provider.execute_events("mcp_a", "{}", ctx, sink);

    CHECK_EQ(std::size_t(3), events.size());
    CHECK(events[0].kind == shell_provider::ToolExecutionEvent::Kind::TargetResolved);
    CHECK_EQ(std::string("srv"), events[0].text);
    CHECK(events[1].kind == shell_provider::ToolExecutionEvent::Kind::Progress);
    CHECK_EQ(std::string("Calling MCP tool"), events[1].text);
    CHECK(events[2].kind == shell_provider::ToolExecutionEvent::Kind::Completed);
    CHECK_EQ(std::string("done"), events[2].text);
    CHECK(!events[2].is_error);
    CHECK_EQ(1, fixture.registry.execute_calls);  // one execute for the Completed event
}

TEST(mcp_execute_events_unknown_tool_skips_target_resolved) {
    Fixture fixture;
    const auto ctx = shell_provider::GenerationContext{};
    fixture.registry.result.text = "Unknown or disabled MCP tool: mcp_gone";
    fixture.registry.result.is_error = true;

    std::vector<shell_provider::ToolExecutionEvent> events;
    const auto sink = [&](const shell_provider::ToolExecutionEvent& event) {
        events.push_back(event);
    };
    fixture.provider.execute_events("mcp_gone", "{}", ctx, sink);

    // No descriptor → no TargetResolved; the registry's error result rides Completed.
    CHECK_EQ(std::size_t(2), events.size());
    CHECK(events[0].kind == shell_provider::ToolExecutionEvent::Kind::Progress);
    CHECK(events[1].kind == shell_provider::ToolExecutionEvent::Kind::Completed);
    CHECK(events[1].is_error);
    CHECK_EQ(std::string("Unknown or disabled MCP tool: mcp_gone"), events[1].text);
}