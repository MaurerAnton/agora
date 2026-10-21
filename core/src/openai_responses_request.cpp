// Port of api/openai/OpenAiRequestValidator.kt.
//
// The chat messages become Responses input items (the stored reasoning items are replayed only
// when the provider and the model match), the tool definitions become Responses tools, and the
// wire-format gate collects every violation before the request may leave the device.
#include "agora/core/openai_responses_request.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::openai_responses {

namespace {

namespace json = agora::core::json;
using json::Kind;
using json::Value;

using openai::OpenAiContentPart;
using openai::OpenAiImageUrl;
using openai::OpenAiMessage;
using openai::OpenAiRequestFunction;
using openai::OpenAiRequestToolCall;
using openai::OpenAiReasoning;
using openai::ToolDefinition;
using openai::ToolParameters;

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool is_blank(const std::string& text) {
    for (unsigned char c : text) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string remove_prefix(const std::string& text, std::string_view prefix) {
    if (text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0) {
        return text.substr(prefix.size());
    }
    return text;
}

/** model/ModelId.kt::ModelId.parse — the model name half decides the replay check. */
std::string model_id_model_name(const std::string& prefixed) {
    const std::size_t separator = prefixed.find(':');
    if (separator != std::string::npos) return prefixed.substr(separator + 1);
    return prefixed;
}

const std::regex& safe_wire_tool_name() {
    static const std::regex pattern("[A-Za-z0-9_-]{1,64}");
    return pattern;
}

const std::regex& safe_wire_tool_call_id() {
    static const std::regex pattern("[A-Za-z0-9_-]{1,128}");
    return pattern;
}

bool matches(const std::regex& pattern, const std::string& text) {
    return std::regex_match(text, pattern);
}

bool is_json_object(const std::string& raw) {
    const json::ParseResult parsed = json::parse(raw);
    return parsed.value.has_value() && parsed.value->kind == Kind::Object;
}

// ----------------------------------------------------------------- item encoding

Value encode_content(const ResponseInputContent& content) {
    Value out;
    out.kind = Kind::Object;
    out.members.emplace_back("type", [&] {
        Value value;
        value.kind = Kind::String;
        value.text = content.type;
        return value;
    }());
    if (content.text.has_value()) {
        Value value;
        value.kind = Kind::String;
        value.text = *content.text;
        out.members.emplace_back("text", std::move(value));
    }
    if (content.image_url.has_value()) {
        Value value;
        value.kind = Kind::String;
        value.text = *content.image_url;
        out.members.emplace_back("image_url", std::move(value));
    }
    if (content.detail.has_value()) {
        Value value;
        value.kind = Kind::String;
        value.text = *content.detail;
        out.members.emplace_back("detail", std::move(value));
    }
    return out;
}

/** validationJson (encodeDefaults = false): only the fields that carry a value are written. */
Value encode_item(const ResponseInputItem& item) {
    Value out;
    out.kind = Kind::Object;
    out.members.emplace_back("type", [&] {
        Value value;
        value.kind = Kind::String;
        value.text = item.type;
        return value;
    }());
    auto put_string = [&out](const char* key, const std::optional<std::string>& value) {
        if (!value.has_value()) return;
        Value entry;
        entry.kind = Kind::String;
        entry.text = *value;
        out.members.emplace_back(key, std::move(entry));
    };
    put_string("id", item.id);
    put_string("role", item.role);
    if (item.content.has_value()) {
        Value array;
        array.kind = Kind::Array;
        for (const ResponseInputContent& content : *item.content) {
            array.elements.push_back(encode_content(content));
        }
        out.members.emplace_back("content", std::move(array));
    }
    if (item.summary.has_value()) out.members.emplace_back("summary", *item.summary);
    put_string("encrypted_content", item.encrypted_content);
    put_string("call_id", item.call_id);
    put_string("name", item.name);
    put_string("arguments", item.arguments);
    if (item.output.has_value()) out.members.emplace_back("output", *item.output);
    return out;
}

// ----------------------------------------------------------------- item decoding

struct DecodedItem {
    std::string type;
    std::optional<std::string> id;
    std::optional<std::string> role;
    std::optional<std::vector<ResponseInputContent>> content;
    std::optional<json::Value> summary;
    std::optional<std::string> encrypted_content;
    std::optional<std::string> call_id;
    std::optional<std::string> name;
    std::optional<std::string> arguments;
    std::optional<json::Value> output;
};

std::optional<std::string> string_field(const Value& object, const char* name) {
    for (const json::Member& member : object.members) {
        if (member.key != name) continue;
        if (member.value.kind != Kind::String) return std::nullopt;
        return member.value.text;
    }
    return std::nullopt;
}

bool has_member(const Value& object, const char* name) {
    for (const json::Member& member : object.members) {
        if (member.key == name) return true;
    }
    return false;
}

/** toResponseInputItem: the three typed items are decoded, everything else stays opaque. */
bool decode_item(const Value& raw, DecodedItem* out) {
    if (raw.kind != Kind::Object) return false;
    const std::optional<std::string> type = string_field(raw, "type");
    if (!type.has_value()) return false;
    out->type = *type;
    if (out->type == "message" || out->type == "function_call" ||
        out->type == "function_call_output") {
        out->id = string_field(raw, "id");
        out->role = string_field(raw, "role");
        if (has_member(raw, "content")) {
            const Value* content = nullptr;
            for (const json::Member& member : raw.members) {
                if (member.key == "content") content = &member.value;
            }
            if (content != nullptr && content->kind == Kind::Array) {
                std::vector<ResponseInputContent> parts;
                for (const Value& element : content->elements) {
                    if (element.kind != Kind::Object) return false;
                    ResponseInputContent part;
                    part.type = string_field(element, "type").value_or("");
                    part.text = string_field(element, "text");
                    part.image_url = string_field(element, "image_url");
                    part.detail = string_field(element, "detail");
                    parts.push_back(std::move(part));
                }
                out->content = std::move(parts);
            }
        }
        for (const json::Member& member : raw.members) {
            if (member.key == "summary") out->summary = member.value;
            if (member.key == "encrypted_content" && member.value.kind == Kind::String) {
                out->encrypted_content = member.value.text;
            }
            if (member.key == "call_id" && member.value.kind == Kind::String) {
                out->call_id = member.value.text;
            }
            if (member.key == "name" && member.value.kind == Kind::String) {
                out->name = member.value.text;
            }
            if (member.key == "arguments" && member.value.kind == Kind::String) {
                out->arguments = member.value.text;
            }
            if (member.key == "output") out->output = member.value;
        }
        return true;
    }
    out->id = string_field(raw, "id");
    out->call_id = string_field(raw, "call_id");
    out->name = string_field(raw, "name");
    out->arguments = string_field(raw, "arguments");
    for (const json::Member& member : raw.members) {
        if (member.key == "output") out->output = member.value;
    }
    return true;
}

void validate_response_content(const ResponseInputContent& content,
                               const std::optional<std::string>& role, const std::string& location,
                               std::vector<std::string>* violations) {
    if (content.type == "input_text") {
        if (role.has_value() && *role == "assistant") {
            violations->push_back(location + " assistant text must use output_text");
        }
        if (!content.text.has_value() || content.image_url.has_value() ||
            content.detail.has_value()) {
            violations->push_back(location + " is not valid input_text");
        }
        return;
    }
    if (content.type == "output_text") {
        if (!role.has_value() || *role != "assistant") {
            violations->push_back(location + " " + role.value_or("null") +
                                  " text must use input_text");
        }
        if (!content.text.has_value() || content.image_url.has_value() ||
            content.detail.has_value()) {
            violations->push_back(location + " is not valid output_text");
        }
        return;
    }
    if (content.type == "input_image") {
        if (role.has_value() && *role == "assistant") {
            violations->push_back(location + " assistant message cannot contain input_image");
        }
        if (!content.image_url.has_value() || is_blank(*content.image_url) ||
            content.text.has_value() || !content.detail.has_value() ||
            is_blank(*content.detail)) {
            violations->push_back(location + " is not valid input_image");
        }
        return;
    }
    violations->push_back(location + " has unsupported type " + content.type);
}

}  // namespace

RequestFormatException::RequestFormatException(std::string provider,
                                               std::vector<std::string> violations)
    : std::runtime_error(provider + " request validation failed: " + [&] {
          std::string joined;
          for (std::size_t i = 0; i < violations.size(); ++i) {
              if (i != 0) joined += "; ";
              joined += violations[i];
          }
          return joined;
      }()),
      provider_(std::move(provider)),
      violations_(std::move(violations)) {}

std::vector<json::Value> to_responses_input(const std::vector<OpenAiMessage>& messages,
                                           const std::optional<std::string>& provider_name,
                                           const std::optional<std::string>& target_model) {
    std::vector<json::Value> out;
    for (const OpenAiMessage& message : messages) {
        if (message.role == "tool") {
            ResponseInputItem item;
            item.type = "function_call_output";
            item.call_id = message.tool_call_id;
            std::string output;
            if (message.content.has_value()) {
                for (const OpenAiContentPart& part : *message.content) {
                    output += part.text.value_or("");
                }
            }
            Value output_value;
            output_value.kind = Kind::String;
            output_value.text = output;
            item.output = output_value;
            out.push_back(encode_item(item));
            continue;
        }

        std::vector<ResponseInputContent> content;
        if (message.content.has_value()) {
            for (const OpenAiContentPart& part : *message.content) {
                ResponseInputContent entry;
                if (part.type == "image_url") {
                    entry.type = "input_image";
                    entry.image_url = part.image_url.has_value() ? part.image_url->url
                                                                  : std::optional<std::string>();
                    entry.detail = "auto";
                } else {
                    entry.type = message.role == "assistant" ? "output_text" : "input_text";
                    entry.text = part.text.value_or("");
                }
                content.push_back(std::move(entry));
            }
        }
        if (!content.empty()) {
            ResponseInputItem item;
            item.type = "message";
            item.role = message.role;
            item.content = std::move(content);
            out.push_back(encode_item(item));
        }

        // The stored reasoning items belong to the model that produced them.
        const std::optional<std::string> stored_model =
            message.response_output_item_model.has_value()
                ? std::optional<std::string>(trimmed(*message.response_output_item_model))
                : std::nullopt;
        const std::optional<std::string> wanted_model =
            target_model.has_value() ? std::optional<std::string>(trimmed(*target_model))
                                      : std::nullopt;
        bool replay = false;
        if (provider_name.has_value() && message.response_output_item_provider == provider_name &&
            stored_model.has_value() && !stored_model->empty() && wanted_model.has_value() &&
            !wanted_model->empty()) {
            const std::string stored = remove_prefix(model_id_model_name(*stored_model), "models/");
            const std::string wanted = remove_prefix(model_id_model_name(*wanted_model), "models/");
            replay = ascii_lower(stored) == ascii_lower(wanted);
        }
        std::vector<json::Value> replayed;
        if (replay && message.response_output_items.has_value()) {
            replayed = *message.response_output_items;
        }
        for (const json::Value& item : replayed) out.push_back(item);
        if (replayed.empty() && message.tool_calls.has_value()) {
            for (const OpenAiRequestToolCall& call : *message.tool_calls) {
                ResponseInputItem item;
                item.type = "function_call";
                item.call_id = call.id;
                item.name = call.function.name;
                item.arguments = call.function.arguments;
                out.push_back(encode_item(item));
            }
        }
    }
    return out;
}

std::vector<ResponseTool> to_responses_tools(const std::vector<ToolDefinition>& tools) {
    std::vector<ResponseTool> out;
    out.reserve(tools.size());
    for (const ToolDefinition& tool : tools) {
        ResponseTool converted;
        converted.name = tool.function.name;
        converted.description = tool.function.description;
        converted.parameters = tool.function.parameters;
        out.push_back(std::move(converted));
    }
    return out;
}

void require_valid_wire_format(const ResponsesRequest& request, const std::string& provider) {
    std::vector<std::string> violations;
    if (is_blank(request.model)) violations.push_back("model is blank");
    if (request.input.empty()) violations.push_back("input is empty");
    if (request.max_output_tokens.has_value() && *request.max_output_tokens <= 0) {
        violations.push_back("max_output_tokens must be positive");
    }
    if (request.top_p.has_value() && (*request.top_p < 0.0f || *request.top_p > 1.0f)) {
        violations.push_back("top_p is outside 0..1");
    }
    if (request.tools.has_value()) {
        const std::vector<ResponseTool>& tools = *request.tools;
        for (std::size_t index = 0; index < tools.size(); ++index) {
            const ResponseTool& tool = tools[index];
            const std::string location = "tools[" + std::to_string(index) + "]";
            if (tool.type == "web_search") {
                if (tool.name.has_value() || tool.description.has_value() ||
                    tool.parameters.has_value()) {
                    violations.push_back(location + " hosted web search has function fields");
                }
            } else if (tool.type == "function") {
                const std::string name = tool.name.value_or("");
                if (!matches(safe_wire_tool_name(), name)) {
                    violations.push_back(location + " name is not wire-safe");
                }
            } else {
                violations.push_back(location + " has unsupported type " + tool.type);
            }
        }
    }

    std::vector<std::string> pending;
    std::set<std::string> seen;
    for (std::size_t index = 0; index < request.input.size(); ++index) {
        const std::string location = "input[" + std::to_string(index) + "]";
        DecodedItem item;
        if (!decode_item(request.input[index], &item)) {
            violations.push_back(location + " is not a supported Responses item");
            continue;
        }
        if (item.type == "message") {
            const std::string role = item.role.value_or("");
            if (role != "system" && role != "user" && role != "assistant" &&
                role != "developer") {
                violations.push_back(location + " has invalid message role " + role);
            }
            if (!pending.empty()) {
                violations.push_back(location + " interrupts pending tool results");
            }
            if (!item.content.has_value() || item.content->empty()) {
                violations.push_back(location + " content is empty");
            }
            if (item.content.has_value()) {
                for (std::size_t content_index = 0; content_index < item.content->size();
                     ++content_index) {
                    validate_response_content(
                        (*item.content)[content_index], item.role,
                        location + ".content[" + std::to_string(content_index) + "]", &violations);
                }
            }
            if ((item.id.has_value() && role != "assistant") || item.summary.has_value() ||
                item.encrypted_content.has_value() || item.call_id.has_value() ||
                item.name.has_value() || item.arguments.has_value() || item.output.has_value()) {
                violations.push_back(location + " message carries unrelated fields");
            }
        } else if (item.type == "reasoning") {
            if (!item.id.has_value() || is_blank(*item.id)) {
                violations.push_back(location + " reasoning id is blank");
            }
            if (item.role.has_value() || item.content.has_value() || item.call_id.has_value() ||
                item.name.has_value() || item.arguments.has_value() || item.output.has_value()) {
                violations.push_back(location + " opaque item carries unrelated fields");
            }
        } else if (item.type == "function_call") {
            const std::string call_id = item.call_id.value_or("");
            if (is_blank(call_id) || !matches(safe_wire_tool_call_id(), call_id)) {
                violations.push_back(location + " call_id is not wire-safe");
            } else {
                if (seen.count(call_id) != 0) {
                    violations.push_back(location + " reuses call_id " + call_id);
                }
                seen.insert(call_id);
                pending.push_back(call_id);
            }
            const std::string name = item.name.value_or("");
            if (is_blank(name) || !matches(safe_wire_tool_name(), name)) {
                violations.push_back(location + " name is not wire-safe");
            }
            if (!item.arguments.has_value() || !is_json_object(*item.arguments)) {
                violations.push_back(location + " arguments are not a JSON object");
            }
            if (item.summary.has_value() || item.encrypted_content.has_value() ||
                item.role.has_value() || item.content.has_value() || item.output.has_value()) {
                violations.push_back(location + " function call carries unrelated fields");
            }
        } else if (item.type == "function_call_output") {
            const std::string call_id = item.call_id.value_or("");
            if (is_blank(call_id)) {
                violations.push_back(location + " call_id is blank");
            } else {
                const auto found = std::find(pending.begin(), pending.end(), call_id);
                if (found == pending.end()) {
                    violations.push_back(location + " does not match a pending function call");
                } else {
                    pending.erase(found);
                }
            }
            if (!item.output.has_value()) violations.push_back(location + " output is absent");
            if (item.summary.has_value() || item.encrypted_content.has_value() ||
                item.role.has_value() || item.content.has_value() || item.name.has_value() ||
                item.arguments.has_value()) {
                violations.push_back(location + " function output carries unrelated fields");
            }
        } else {
            if (!item.id.has_value() || is_blank(*item.id)) {
                violations.push_back(location + " opaque item id is blank");
            }
            if (item.role.has_value() || item.content.has_value() || item.summary.has_value() ||
                item.encrypted_content.has_value() || item.call_id.has_value() ||
                item.name.has_value() || item.arguments.has_value() || item.output.has_value()) {
                violations.push_back(location + " opaque item carries unrelated fields");
            }
        }
    }
    if (!pending.empty()) violations.push_back("function calls are missing outputs");

    bool ends_correctly = false;
    if (!request.input.empty()) {
        DecodedItem last;
        if (decode_item(request.input.back(), &last)) {
            ends_correctly = (last.type == "message" && last.role.value_or("") == "user") ||
                              last.type == "function_call_output";
        }
    }
    if (!ends_correctly) violations.push_back("input does not end in user/function output");

    if (violations.empty()) return;
    // violations.distinct(): the first occurrence wins, the order is kept.
    std::vector<std::string> distinct;
    std::set<std::string> seen_violations;
    for (const std::string& violation : violations) {
        if (seen_violations.insert(violation).second) distinct.push_back(violation);
    }
    throw RequestFormatException(provider, std::move(distinct));
}

}  // namespace agora::core::openai_responses
