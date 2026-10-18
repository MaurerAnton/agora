#include "agora/core/mcp_protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/sha256.hpp"
#include "agora/core/unicode.hpp"

namespace agora::core::mcp {
namespace {

namespace json = agora::core::json;
namespace openai = agora::core::openai;

constexpr char kStreamableHttpProtocolVersion[] = "2025-11-25";
constexpr char kLegacySseProtocolVersion[] = "2024-11-05";

const std::vector<std::string>& streamable_http_versions() {
    static const std::vector<std::string> versions = {
        kStreamableHttpProtocolVersion, "2025-06-18", "2025-03-26", kLegacySseProtocolVersion};
    return versions;
}

[[noreturn]] void throw_io(std::string message) {
    throw openai::ProviderRequestError{"IOException", std::move(message)};
}

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value json_object() {
    json::Value value;
    value.kind = json::Kind::Object;
    return value;
}

const json::Value* member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;  // last duplicate wins
    }
    return found;
}

bool is_null_literal(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

/** JsonPrimitive.contentOrNull: primitives carry content, JsonNull does not. */
std::optional<std::string> content_or_null(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (is_null_literal(value)) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return std::nullopt;
}

std::optional<std::string> primitive_content(const json::Value& object, std::string_view name) {
    return content_or_null(member(object, name));
}

const json::Value* as_object_or_null(const json::Value* value) {
    if (value == nullptr) return nullptr;
    return value->kind == json::Kind::Object ? value : nullptr;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool is_surrogate(std::uint16_t unit) { return unit >= 0xD800 && unit <= 0xDFFF; }

/** Char.isLetterOrDigit() via the generated Unicode tables (ASCII digits included). */
bool is_letter_or_digit(char32_t cp) {
    return unicode::is_letter(cp) || unicode::is_number(cp);
}

/** Decodes UTF-8 into code points (well-formed input; replacements are out of the pinned corpus). */
std::vector<char32_t> decode_utf8(std::string_view text) {
    std::vector<char32_t> points;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F;
            extra = 2;
        } else {
            cp = lead & 0x07;
            extra = 3;
        }
        ++i;
        for (std::size_t k = 0; k < extra && i < text.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        }
        points.push_back(cp);
    }
    return points;
}

/** The UTF-16 code units of the text — Kotlin's Char iteration domain (a surrogate pair is two). */
std::vector<std::uint16_t> utf16_units(std::string_view text) {
    std::vector<std::uint16_t> units;
    for (char32_t cp : decode_utf8(text)) {
        if (cp > 0xFFFF) {
            const char32_t value = cp - 0x10000;
            units.push_back(static_cast<std::uint16_t>(0xD800 + (value >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xDC00 + (value & 0x3FF)));
        } else {
            units.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    return units;
}

/** Encodes one code point back to UTF-8. */
std::string encode_utf8(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

/** Encodes one UTF-16 code unit back to UTF-8 (a lone surrogate encodes as U+FFFD). */
std::string encode_unit(std::uint16_t unit) {
    if (unit >= 0xD800 && unit <= 0xDFFF) return "\xEF\xBF\xBD";  // U+FFFD
    return encode_utf8(static_cast<char32_t>(unit));
}

/** String.take(n) in UTF-16 code units (astral splits are left alone, as the corpus pins). */
std::string take_utf16(std::string_view text, std::size_t units) {
    const std::vector<std::uint16_t> all = utf16_units(text);
    const std::size_t count = std::min(units, all.size());
    std::string out;
    for (std::size_t i = 0; i < count; ++i) out += encode_unit(all[i]);
    return out;
}

/** UTF-16 length of an already-ASCII string. */
std::size_t utf16_length(std::string_view text) {
    std::size_t units = 0;
    for (char32_t cp : decode_utf8(text)) units += cp > 0xFFFF ? 2 : 1;
    return units;
}

/** Ordered key → value list with LinkedHashMap.put semantics: last value, first position. */
std::vector<std::pair<std::string, json::Value>> ordered_members(const json::Value& object) {
    std::vector<std::pair<std::string, json::Value>> out;
    for (const json::Member& entry : object.members) {
        bool replaced = false;
        for (auto& [key, value] : out) {
            if (key == entry.key) {
                value = entry.value;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.emplace_back(entry.key, entry.value);
    }
    return out;
}

/** buildMap.put on an existing key replaces in place, otherwise appends. */
void map_put(std::vector<std::pair<std::string, json::Value>>& map, std::string key,
             json::Value value) {
    for (auto& [existing, existing_value] : map) {
        if (existing == key) {
            existing_value = std::move(value);
            return;
        }
    }
    map.emplace_back(std::move(key), std::move(value));
}

void map_remove(std::vector<std::pair<std::string, json::Value>>& map, std::string_view key) {
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it->first == key) {
            map.erase(it);
            return;
        }
    }
}

bool map_contains(const std::vector<std::pair<std::string, json::Value>>& map,
                  std::string_view key) {
    for (const auto& [existing, unused] : map) {
        (void)unused;
        if (existing == key) return true;
    }
    return false;
}

std::string infer_schema_type(const json::Value& schema) {
    if (const json::Value* properties = member(schema, "properties");
        properties != nullptr && properties->kind == json::Kind::Object) {
        return "object";
    }
    if (const json::Value* items = member(schema, "items");
        items != nullptr && items->kind == json::Kind::Object) {
        return "array";
    }
    return "string";
}

openai::ToolProperty to_tool_property(const json::Value* schema) {
    openai::ToolProperty property;
    if (schema == nullptr || schema->kind != json::Kind::Object) {
        property.type = "string";
        property.description = "";
        return property;
    }
    std::optional<std::string> declared;
    const json::Value* type = member(*schema, "type");
    if (type != nullptr && (type->kind == json::Kind::String || type->kind == json::Kind::Literal) &&
        !is_null_literal(type)) {
        declared = type->text;
    } else if (type != nullptr && type->kind == json::Kind::Array) {
        for (const json::Value& entry : type->elements) {
            const auto text = content_or_null(&entry);
            if (text && *text != "null") {
                declared = *text;
                break;
            }
        }
    }
    property.type = declared ? *declared : infer_schema_type(*schema);
    property.description = primitive_content(*schema, "description").value_or("");
    const json::Value* items = member(*schema, "items");
    if (items != nullptr && items->kind == json::Kind::Object) {
        property.items = std::make_shared<openai::ToolProperty>(to_tool_property(items));
    }
    return property;
}

/** The MCP input schema as providers receive it (toWireSchema). */
json::Value to_wire_schema(const json::Value& schema, const std::string& type) {
    const json::Value* properties_member = member(schema, "properties");
    const json::Value& properties = (properties_member != nullptr &&
                                     properties_member->kind == json::Kind::Object)
                                        ? *properties_member
                                        : json_object();
    std::vector<std::pair<std::string, json::Value>> properties_map =
        properties.kind == json::Kind::Object ? ordered_members(properties)
                                              : std::vector<std::pair<std::string, json::Value>>{};

    const json::Value* required_member = member(schema, "required");
    bool has_required = false;
    json::Value required = json::Value();
    required.kind = json::Kind::Array;
    if (required_member != nullptr && required_member->kind == json::Kind::Array) {
        has_required = true;
        for (const json::Value& entry : required_member->elements) {
            const auto text = content_or_null(&entry);
            if (text && map_contains(properties_map, *text)) {
                required.elements.push_back(json_string(*text));
            }
        }
    }

    std::vector<std::pair<std::string, json::Value>> map = ordered_members(schema);
    map_remove(map, "$schema");
    map_put(map, "type", json_string(type));
    map_put(map, "properties", properties.kind == json::Kind::Object
                                    ? json::Value(properties)
                                    : json_object());
    if (has_required) map_put(map, "required", std::move(required));

    json::Value out = json_object();
    out.members.reserve(map.size());
    for (auto& [key, value] : map) out.members.push_back({key, std::move(value)});
    return out;
}

openai::ToolParameters to_tool_parameters(const json::Value& schema) {
    openai::ToolParameters parameters;
    if (schema.kind != json::Kind::Object) {
        // toToolParameters is only ever called on a JsonObject; a defensive default keeps the
        // typed shape well-formed for callers that pass something else.
        parameters.type = "object";
        parameters.schema = to_wire_schema(json_object(), "object");
        return parameters;
    }
    const json::Value* properties_member = member(schema, "properties");
    std::vector<std::pair<std::string, const json::Member*>> sorted;
    if (properties_member != nullptr && properties_member->kind == json::Kind::Object) {
        for (const json::Member& entry : properties_member->members) {
            sorted.emplace_back(entry.key, &entry);
        }
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    for (const auto& [name, entry] : sorted) {
        parameters.properties.emplace_back(name, to_tool_property(&entry->value));
    }

    const json::Value* required_member = member(schema, "required");
    if (required_member != nullptr && required_member->kind == json::Kind::Array) {
        for (const json::Value& entry : required_member->elements) {
            const auto text = content_or_null(&entry);
            if (!text) continue;
            bool declared = false;
            for (const auto& [property_name, unused] : parameters.properties) {
                (void)unused;
                if (property_name == *text) {
                    declared = true;
                    break;
                }
            }
            if (declared) parameters.required.push_back(*text);
        }
    }

    const std::string type = primitive_content(schema, "type").value_or("object");
    parameters.type = type;
    parameters.schema = to_wire_schema(schema, type);
    return parameters;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// McpModels.kt
// ---------------------------------------------------------------------------------------------

std::string public_mcp_tool_name(std::string_view server_id, std::string_view remote_name) {
    std::string server_key;
    for (std::uint16_t unit : utf16_units(server_id)) {
        if (is_surrogate(unit)) continue;  // filter(Char::isLetterOrDigit) drops lone surrogates
        if (is_letter_or_digit(static_cast<char32_t>(unit))) {
            server_key += encode_utf8(static_cast<char32_t>(unit));
        }
    }
    server_key = take_utf16(server_key, 10);
    if (json::is_blank(server_key)) server_key = "server";

    std::string tool_key;
    for (std::uint16_t unit : utf16_units(remote_name)) {
        // The map runs per Char, so a surrogate pair becomes two underscores.
        const bool keep =
            !is_surrogate(unit) &&
            (is_letter_or_digit(static_cast<char32_t>(unit)) || unit == U'_' || unit == U'-');
        tool_key += keep ? encode_utf8(static_cast<char32_t>(unit)) : std::string("_");
    }
    std::size_t begin = 0;
    std::size_t end = tool_key.size();
    while (begin < end && tool_key[begin] == '_') ++begin;
    while (end > begin && tool_key[end - 1] == '_') --end;
    tool_key = tool_key.substr(begin, end - begin);
    if (json::is_blank(tool_key)) tool_key = "tool";

    const std::array<std::uint8_t, 32> digest = sha256(remote_name);
    std::string digest_text;
    {
        static constexpr char kHex[] = "0123456789abcdef";
        for (int i = 0; i < 3; ++i) {
            digest_text.push_back(kHex[digest[i] >> 4]);
            digest_text.push_back(kHex[digest[i] & 0x0F]);
        }
    }

    const std::string prefix = "mcp_" + server_key + "_";
    const std::string suffix = "_" + digest_text;
    const std::int64_t budget =
        std::max<std::int64_t>(1, 64 - static_cast<std::int64_t>(utf16_length(prefix)) -
                                      static_cast<std::int64_t>(utf16_length(suffix)));
    return prefix + take_utf16(tool_key, static_cast<std::size_t>(budget)) + suffix;
}

ToolDefinitionProjection project_tool_definition(std::string_view public_name,
                                                std::string_view server_name,
                                                const RemoteTool& remote) {
    std::string description =
        json::is_blank(remote.description) ? "MCP tool " + remote.name : remote.description;
    description += "\n\nProvided by MCP server: ";
    description += server_name;

    ToolDefinitionProjection projection;
    projection.definition.function.name = std::string(public_name);
    projection.definition.function.description = std::move(description);
    projection.definition.function.parameters = to_tool_parameters(remote.input_schema);
    projection.schema = projection.definition.function.parameters.schema.value_or(json_object());
    return projection;
}

// ---------------------------------------------------------------------------------------------
// McpProtocolClient.kt
// ---------------------------------------------------------------------------------------------

CallPayload parse_call_payload(const json::Value& result) {
    CallPayload payload;
    const json::Value* content = result.kind == json::Kind::Object ? member(result, "content")
                                                                  : nullptr;
    if (content != nullptr && content->kind == json::Kind::Array) {
        for (const json::Value& element : content->elements) {
            if (element.kind != json::Kind::Object) continue;
            const std::optional<std::string> type = primitive_content(element, "type");
            if (type && *type == "text") {
                if (auto text = primitive_content(element, "text")) payload.text_parts.push_back(*text);
            } else if (type && *type == "image") {
                const auto data = primitive_content(element, "data");
                const auto mime = primitive_content(element, "mimeType");
                if (data && !json::is_blank(*data) && mime && !json::is_blank(*mime)) {
                    payload.images.push_back(ImagePayload{*data, *mime});
                }
            } else if (type && *type == "resource") {
                const json::Value* resource = as_object_or_null(member(element, "resource"));
                const auto mime =
                    resource ? primitive_content(*resource, "mimeType") : std::nullopt;
                const auto blob = resource ? primitive_content(*resource, "blob") : std::nullopt;
                const auto text = resource ? primitive_content(*resource, "text") : std::nullopt;
                if (blob && !json::is_blank(*blob) && mime &&
                    mime->rfind("image/", 0) == 0) {
                    payload.images.push_back(ImagePayload{*blob, *mime});
                } else if (text && !json::is_blank(*text)) {
                    payload.text_parts.push_back(*text);
                }
            }
        }
    }
    if (result.kind == json::Kind::Object) {
        if (const json::Value* structured = member(result, "structuredContent");
            structured != nullptr && !is_null_literal(structured)) {
            payload.structured_content = *structured;
        }
        if (auto flag = primitive_content(result, "isError")) payload.is_error = *flag == "true";
    }
    return payload;
}

ToolsPage parse_tools_page(const json::Value& result) {
    const json::Value* tools = result.kind == json::Kind::Object ? member(result, "tools")
                                                                  : nullptr;
    if (tools == nullptr || tools->kind != json::Kind::Array) {
        throw_io("MCP tools/list returned no tools array");
    }
    ToolsPage page;
    for (const json::Value& element : tools->elements) {
        if (element.kind != json::Kind::Object) continue;
        const auto name = primitive_content(element, "name");
        if (!name || json::is_blank(*name)) continue;
        RemoteTool tool;
        tool.name = *name;
        tool.description = primitive_content(element, "description").value_or("");
        const json::Value* schema = as_object_or_null(member(element, "inputSchema"));
        if (schema != nullptr) {
            tool.input_schema = *schema;
        } else {
            json::Value fallback = json_object();
            fallback.members.push_back({"type", json_string("object")});
            fallback.members.push_back({"properties", json_object()});
            tool.input_schema = std::move(fallback);
        }
        page.tools.push_back(std::move(tool));
    }
    if (const auto cursor = primitive_content(result, "nextCursor");
        cursor && !json::is_blank(*cursor)) {
        page.cursor = *cursor;
    }
    return page;
}

json::Value validate_rpc_response(std::string_view method, std::int64_t id,
                                  const std::optional<json::Value>& response) {
    const std::string method_text(method);
    if (!response) throw_io("MCP " + method_text + " returned an empty response");
    const auto response_id = primitive_content(*response, "id");
    if (!response_id || *response_id != std::to_string(id)) {
        throw_io("MCP " + method_text + " returned mismatched response id");
    }
    const json::Value* error = member(*response, "error");
    if (error != nullptr && !is_null_literal(error)) {
        const json::Value* error_object = as_object_or_null(error);
        std::optional<std::string> message;
        if (error_object != nullptr) message = primitive_content(*error_object, "message");
        if (!message) message = json::to_kotlin_string(*error);
        throw_io("MCP " + method_text + " failed: " + *message);
    }
    const json::Value* result = as_object_or_null(member(*response, "result"));
    if (result == nullptr) throw_io("MCP " + method_text + " returned no result");
    return *result;
}

std::vector<std::string> protocol_candidates(bool streamable_http,
                                             std::string_view current_version) {
    if (!streamable_http) return {kLegacySseProtocolVersion};
    const std::vector<std::string>& versions = streamable_http_versions();
    // dropWhile { it != protocolVersion }: everything from the current version onward.
    std::vector<std::string> candidates;
    std::size_t start = 0;
    while (start < versions.size() && versions[start] != current_version) ++start;
    if (start < versions.size()) {
        candidates.assign(versions.begin() + static_cast<std::ptrdiff_t>(start), versions.end());
    } else {
        candidates = versions;  // ifEmpty { versions }
    }
    return candidates;
}

bool is_unsupported_protocol_version(const std::vector<std::string>& cause_messages) {
    std::string joined;
    for (const std::string& message : cause_messages) {
        if (!joined.empty()) joined.push_back(' ');
        joined += message;
    }
    const std::string text = lower_ascii(joined);
    return text.find("unsupported protocol version") != std::string::npos ||
           text.find("unsupported mcp protocol version") != std::string::npos ||
           text.find("protocol version is not supported") != std::string::npos;
}

std::string negotiated_protocol_version(const json::Value& result) {
    const auto negotiated =
        result.kind == json::Kind::Object ? primitive_content(result, "protocolVersion")
                                          : std::nullopt;
    if (!negotiated) throw_io("MCP initialize returned no protocolVersion");
    const std::vector<std::string>& versions = streamable_http_versions();
    const bool supported =
        std::find(versions.begin(), versions.end(), *negotiated) != versions.end();
    if (!supported) throw_io("Unsupported MCP protocol version: " + *negotiated);
    return *negotiated;
}

}  // namespace agora::core::mcp