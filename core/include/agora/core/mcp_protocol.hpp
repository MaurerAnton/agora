#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::mcp {

// ---------------------------------------------------------------------------------------------
// McpModels.kt — the tool model, its public name and the schema projections. The pieces are
// extracted verbatim by the mcp-protocol stand; this port mirrors them, including the key
// order of the wire schema and the LinkedHashMap in-place replacement rules.
// ---------------------------------------------------------------------------------------------

struct RemoteTool {
    std::string name;
    std::string description;
    json::Value input_schema;
};

/** McpToolDescriptor.asToolDefinition plus the wire schema it attaches to the parameters. */
struct ToolDefinitionProjection {
    openai::ToolDefinition definition;
    json::Value schema;
};

/**
 * publicMcpToolName: `mcp_<serverKey>_<toolKey>_<sha256-prefix>` where serverKey keeps the
 * letters/digits of the server id (first ten), toolKey maps every other character to `_`, trims
 * underscores and falls back to "server"/"tool". The tool key is cut to fit the 64-character
 * budget in UTF-16 code units.
 */
std::string public_mcp_tool_name(std::string_view server_id, std::string_view remote_name);

/** McpToolDescriptor.asToolDefinition: description default + server banner, schema projections. */
ToolDefinitionProjection project_tool_definition(std::string_view public_name,
                                                std::string_view server_name,
                                                const RemoteTool& remote);

// ---------------------------------------------------------------------------------------------
// McpProtocolClient.kt — parseCallPayload (extracted verbatim by the stand).
// ---------------------------------------------------------------------------------------------

struct ImagePayload {
    std::string data;
    std::string mime_type;
};

struct CallPayload {
    std::vector<std::string> text_parts;
    std::vector<ImagePayload> images;
    std::optional<json::Value> structured_content;
    bool is_error = false;
};

/** parseCallPayload: the content branches (text/image/resource) plus structuredContent/isError. */
CallPayload parse_call_payload(const json::Value& result);

// ---------------------------------------------------------------------------------------------
// McpProtocolClient.kt — the inline logic around the transport seam (transcribed by the stand).
// ---------------------------------------------------------------------------------------------

/** The tools/list page mapping; throws ProviderRequestError("IOException") when no array. */
struct ToolsPage {
    std::vector<RemoteTool> tools;
    std::optional<std::string> cursor;
};

ToolsPage parse_tools_page(const json::Value& result);

/** requestLocked's response validation: the id match, the error envelope and the result object. */
json::Value validate_rpc_response(std::string_view method, std::int64_t id,
                                  const std::optional<json::Value>& response);

/** ensureInitializedLocked's candidate order (dropWhile on the current version, wrap if unknown). */
std::vector<std::string> protocol_candidates(bool streamable_http, std::string_view current_version);

/** isUnsupportedMcpProtocolVersion over a cause chain's messages (outer message first). */
bool is_unsupported_protocol_version(const std::vector<std::string>& cause_messages);

/** initializeLocked's result validation: the negotiated version must be one of ours. */
std::string negotiated_protocol_version(const json::Value& result);

}  // namespace agora::core::mcp