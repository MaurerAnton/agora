#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agora/core/mcp_protocol.hpp"
#include "agora/core/shell_tool_provider.hpp"

// tool/McpToolProvider.kt: thin glue over McpRegistry — definitions come from the registry's
// enabled tools (asToolDefinition is descriptor-side logic; the port reuses the pinned
// project_tool_definition), handles/presentation read descriptors, execute() forwards to the
// registry and executeEvents emits the TargetResolved → Progress → Completed sequence.

namespace agora::core::mcp {

/**
 * The McpRegistry seam (mcp/McpRegistry.kt). ToolInfo mirrors the McpToolDescriptor fields the
 * provider reads: publicName, serverName and remote (name/description/inputSchema).
 */
class Registry {
public:
    virtual ~Registry() = default;

    struct ToolInfo {
        std::string public_name;
        std::string server_name;
        RemoteTool remote;
    };

    /** McpRegistry.enabledTools: connected servers' enabled tools, sorted by publicName. */
    virtual std::vector<ToolInfo> enabled_tools() = 0;

    /** McpRegistry.descriptor: the enabled descriptor for this public name, if any. */
    virtual std::optional<ToolInfo> descriptor(const std::string& public_name) = 0;

    /** McpRegistry.execute — unknown/disabled tools fold into the result's error side. */
    virtual shell_provider::ToolExecutionResult execute(const std::string& public_name,
                                                        const std::string& arguments) = 0;
};

class McpToolProvider final : public shell_provider::ToolProvider {
public:
    explicit McpToolProvider(Registry& registry);

    /** `registry.enabledTools().map { it.asToolDefinition() }` — no ctx gate (the registry is). */
    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    /** `registry.execute(name, arguments).text`. */
    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    /** TargetResolved(descriptor.serverName)? → Progress("Calling MCP tool") → Completed. */
    void execute_events(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx,
                        const shell_provider::ToolEventSink& sink) override;

    bool handles(const std::string& name) const override;

    std::optional<shell_provider::ToolPresentationMetadata> presentation_metadata(
        const std::string& name) const override;

private:
    Registry& registry_;
};

}  // namespace agora::core::mcp