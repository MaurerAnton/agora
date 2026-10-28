#include "agora/core/mcp_tool_provider.hpp"

#include <utility>

namespace agora::core::mcp {

McpToolProvider::McpToolProvider(Registry& registry) : registry_(registry) {}

bool McpToolProvider::handles(const std::string& name) const {
    return registry_.descriptor(name).has_value();
}

std::vector<openai::ToolDefinition> McpToolProvider::definitions(
    const shell_provider::GenerationContext& /*ctx*/) const {
    std::vector<openai::ToolDefinition> definitions;
    for (const Registry::ToolInfo& tool : registry_.enabled_tools()) {
        definitions.push_back(
            project_tool_definition(tool.public_name, tool.server_name, tool.remote).definition);
    }
    return definitions;
}

std::string McpToolProvider::execute(const std::string& name, const std::string& arguments,
                                     const shell_provider::GenerationContext& /*ctx*/) {
    return registry_.execute(name, arguments).text;
}

void McpToolProvider::execute_events(const std::string& name, const std::string& arguments,
                                     const shell_provider::GenerationContext& /*ctx*/,
                                     const shell_provider::ToolEventSink& sink) {
    // Kotlin: presentationMetadata(name)?.target?.let { emit(TargetResolved(it)) }
    if (const std::optional<Registry::ToolInfo> descriptor = registry_.descriptor(name)) {
        sink(shell_provider::ToolExecutionEvent::target_resolved(descriptor->server_name));
    }
    sink(shell_provider::ToolExecutionEvent::progress("Calling MCP tool"));
    sink(shell_provider::ToolExecutionEvent::completed(registry_.execute(name, arguments)));
}

std::optional<shell_provider::ToolPresentationMetadata> McpToolProvider::presentation_metadata(
    const std::string& name) const {
    const std::optional<Registry::ToolInfo> descriptor = registry_.descriptor(name);
    if (!descriptor.has_value()) return std::nullopt;
    return shell_provider::ToolPresentationMetadata{descriptor->remote.name,
                                                    descriptor->server_name};
}

}  // namespace agora::core::mcp