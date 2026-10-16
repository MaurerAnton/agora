// Golden parity test for the C++ message-preparation port.
//
// Replays every scenario under core/tools/golden/message-preparation/scenarios through the C++
// port and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format, escaping and the message-dump JSON shapes mirror the reference driver
// (core/tools/golden/message-preparation/MessagePreparationTraceDump.kt) exactly.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/bpe_vocabulary.hpp"
#include "agora/core/json.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/token_count.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace prep = agora::core::prep;
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

/** The shipped o200k vocabulary, loaded once; the family counter counts through the pinned core. */
const agora::core::BpeVocabulary& o200k_vocabulary() {
    static const agora::core::BpeVocabulary vocabulary = [] {
        const std::string text = read_file(AGORA_FIXTURE_PREP_VOCAB);
        return agora::core::BpeVocabulary::parse_tiktoken(text);
    }();
    return vocabulary;
}

prep::TextTokenCounter o200k_counter() {
    return [](std::string_view text) {
        return agora::core::bpe_token_count(text, o200k_vocabulary());
    };
}

// -- scenario decoding ------------------------------------------------------------------------------

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
    if (value->kind != json::Kind::String) scenario_fail(std::string(name), "expected a JSON string");
    return value->text;
}

std::string string_or(const json::Value& object, std::string_view name, const std::string& fallback) {
    const auto value = optional_string(object, name);
    return value ? *value : fallback;
}

std::string require_string(const json::Value& object, std::string_view name) {
    const auto value = optional_string(object, name);
    if (!value) scenario_fail(std::string(name), "is required");
    return *value;
}

bool bool_or(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal || (value->text != "true" && value->text != "false")) {
        scenario_fail(std::string(name), "expected a JSON boolean");
    }
    return value->text == "true";
}

std::int32_t int_or(const json::Value& object, std::string_view name, std::int32_t fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal || value->text.find_first_of(".eE") != std::string::npos) {
        scenario_fail(std::string(name), "expected an Int");
    }
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value->text.data(), value->text.data() + value->text.size(),
                                        parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
        scenario_fail(std::string(name), "expected an Int");
    }
    return parsed;
}

std::int64_t long_or(const json::Value& object, std::string_view name, std::int64_t fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected a Long");
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value->text.data(), value->text.data() + value->text.size(),
                                        parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
        scenario_fail(std::string(name), "expected a Long");
    }
    return parsed;
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

prep::Participant parse_participant(const std::string& value) {
    if (value == "USER") return prep::Participant::User;
    if (value == "MODEL") return prep::Participant::Model;
    if (value == "ERROR") return prep::Participant::Error;
    scenario_fail("participant", "unknown participant: " + value);
}

prep::MessageStatus parse_status(const std::string& value) {
    if (value == "TRANSCRIBING") return prep::MessageStatus::Transcribing;
    if (value == "SENDING") return prep::MessageStatus::Sending;
    if (value == "THINKING") return prep::MessageStatus::Thinking;
    if (value == "TOOL_CALLING") return prep::MessageStatus::ToolCalling;
    if (value == "SUCCESS") return prep::MessageStatus::Success;
    if (value == "STOPPED") return prep::MessageStatus::Stopped;
    if (value == "ERROR") return prep::MessageStatus::Error;
    scenario_fail("status", "unknown status: " + value);
}

std::string participant_name(prep::Participant participant) {
    switch (participant) {
        case prep::Participant::User: return "USER";
        case prep::Participant::Model: return "MODEL";
        case prep::Participant::Error: return "ERROR";
    }
    return "USER";
}

std::string status_name(prep::MessageStatus status) {
    switch (status) {
        case prep::MessageStatus::Transcribing: return "TRANSCRIBING";
        case prep::MessageStatus::Sending: return "SENDING";
        case prep::MessageStatus::Thinking: return "THINKING";
        case prep::MessageStatus::ToolCalling: return "TOOL_CALLING";
        case prep::MessageStatus::Success: return "SUCCESS";
        case prep::MessageStatus::Stopped: return "STOPPED";
        case prep::MessageStatus::Error: return "ERROR";
    }
    return "SUCCESS";
}

std::optional<std::vector<prep::MessageSegment>> decode_segments(const json::Value* value) {
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::Array) scenario_fail("segments", "expected an array");
    std::vector<prep::MessageSegment> out;
    for (const json::Value& entry : value->elements) {
        if (entry.kind != json::Kind::Object) scenario_fail("segments", "expected objects");
        prep::MessageSegment segment;
        segment.type = require_string(entry, "type");
        segment.content = string_or(entry, "content", "");
        segment.tool_name = optional_string(entry, "toolName");
        segment.tool_args = optional_string(entry, "toolArgs");
        segment.tool_result = optional_string(entry, "toolResult");
        segment.tool_call_id = optional_string(entry, "toolCallId");
        segment.signature = optional_string(entry, "signature");
        segment.signature_provider = optional_string(entry, "signatureProvider");
        segment.tool_transcription = optional_string(entry, "toolTranscription");
        segment.error_code = optional_string(entry, "errorCode");
        if (const json::Value* items = field(entry, "responseOutputItems");
            items != nullptr && !is_json_null(items)) {
            if (items->kind != json::Kind::Array) scenario_fail("segments", "items must be an array");
            segment.response_output_items = items->elements;
        }
        segment.response_output_item_provider =
            optional_string(entry, "responseOutputItemProvider");
        out.push_back(std::move(segment));
    }
    return out;
}

std::optional<prep::ToolCallData> decode_tool_call(const json::Value* value) {
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::Object) scenario_fail("toolCall", "expected an object");
    prep::ToolCallData call;
    call.tool_name = require_string(*value, "toolName");
    call.arguments = require_string(*value, "arguments");
    call.result = require_string(*value, "result");
    call.signature = optional_string(*value, "signature");
    call.tool_call_id = optional_string(*value, "toolCallId");
    call.display_name = optional_string(*value, "displayName");
    call.result_text = optional_string(*value, "resultText");
    call.structured_result = optional_string(*value, "structuredResult");
    call.transcription = optional_string(*value, "transcription");
    if (const json::Value* items = field(*value, "responseOutputItems");
        items != nullptr && !is_json_null(items)) {
        if (items->kind != json::Kind::Array) scenario_fail("toolCall", "items must be an array");
        call.response_output_items = items->elements;
    }
    call.response_output_item_provider = optional_string(*value, "responseOutputItemProvider");
    return call;
}

std::optional<prep::AttachmentMeta> decode_attachments(const json::Value* value) {
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::Object) scenario_fail("attachmentMeta", "expected an object");
    prep::AttachmentMeta meta;
    const json::Value* items = field(*value, "items");
    if (items != nullptr && !is_json_null(items)) {
        if (items->kind != json::Kind::Array) scenario_fail("attachmentMeta", "items must be an array");
        for (const json::Value& entry : items->elements) {
            if (entry.kind != json::Kind::Object) scenario_fail("attachmentMeta", "items must be objects");
            prep::AttachmentItem item;
            item.type = require_string(entry, "type");
            if (const json::Value* index = field(entry, "imageIndex"); index && !is_json_null(index)) {
                item.image_index = int_or(entry, "imageIndex", 0);
            }
            if (const json::Value* pages = field(entry, "pageCount"); pages && !is_json_null(pages)) {
                item.page_count = int_or(entry, "pageCount", 0);
            }
            if (const json::Value* size = field(entry, "fileSize"); size && !is_json_null(size)) {
                item.file_size = long_or(entry, "fileSize", 0);
            }
            if (const json::Value* width = field(entry, "pixelWidth"); width && !is_json_null(width)) {
                item.pixel_width = int_or(entry, "pixelWidth", 0);
            }
            if (const json::Value* height = field(entry, "pixelHeight"); height && !is_json_null(height)) {
                item.pixel_height = int_or(entry, "pixelHeight", 0);
            }
            meta.items.push_back(std::move(item));
        }
    }
    return meta;
}

prep::ChatMessage build_message(const json::Value& message) {
    prep::ChatMessage built;
    built.id = require_string(message, "id");
    built.parent_id = optional_string(message, "parentId");
    built.participant = parse_participant(require_string(message, "participant"));
    built.status = parse_status(string_or(message, "status", "SUCCESS"));
    built.text = string_or(message, "text", "");
    built.images = string_list(field(message, "images"));
    built.thoughts = optional_string(message, "thoughts");
    built.thought_title = optional_string(message, "thoughtTitle");
    built.token_count = int_or(message, "tokenCount", 0);
    built.model_name = optional_string(message, "modelName");
    built.timestamp = long_or(message, "timestamp", 0);
    built.run_id = optional_string(message, "runId");
    built.run_sequence = [&]() -> std::optional<std::int64_t> {
        const json::Value* value = field(message, "runSequence");
        if (value == nullptr || is_json_null(value)) return std::nullopt;
        return long_or(message, "runSequence", 0);
    }();
    built.segments = decode_segments(field(message, "segments"));
    built.tool_call = decode_tool_call(field(message, "toolCall"));
    built.attachment_meta = decode_attachments(field(message, "attachmentMeta"));
    return built;
}

// -- tools (estimateFixed modes): the app's programmatic ToolDefinition shape ------------------------

openai::ToolProperty build_tool_property(const json::Value& value, const std::string& name) {
    if (value.kind != json::Kind::Object) scenario_fail("tool property " + name, "expected an object");
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
        property.items =
            std::make_shared<openai::ToolProperty>(build_tool_property(*items, name + "[]"));
    }
    if (const json::Value* properties = field(value, "properties");
        properties != nullptr && !is_json_null(properties)) {
        if (properties->kind != json::Kind::Object) {
            scenario_fail("tool property " + name, "properties must be an object");
        }
        std::vector<std::pair<std::string, openai::ToolProperty>> nested;
        for (const json::Member& entry : properties->members) {
            nested.emplace_back(entry.key, build_tool_property(entry.value, entry.key));
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
            parameters.properties.emplace_back(entry.key, build_tool_property(entry.value, entry.key));
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

std::vector<openai::ToolDefinition> build_tools(const json::Value* raw_tools) {
    std::vector<openai::ToolDefinition> tools;
    if (raw_tools == nullptr || is_json_null(raw_tools)) return tools;
    if (raw_tools->kind != json::Kind::Array) scenario_fail("tools", "expected an array");
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
        if (name == nullptr || name->kind != json::Kind::String) scenario_fail("tools", "tool without name");
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

// -- trace projection: the driver's dump DTOs, kotlinx-exact ---------------------------------------

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

void append_dump_member(std::string& out, bool& first, std::string_view name) {
    if (!first) out.push_back(',');
    first = false;
    out.push_back('"');
    out += name;
    out.push_back('"');
    out.push_back(':');
}

/** SegmentDump: type, content, toolName, toolArgs, toolResult, toolCallId, signature,
 *  signatureProvider, toolTranscription, errorCode, responseOutputItems, responseOutputItemProvider. */
std::string dump_segments(const prep::ChatMessage& message) {
    std::string out = "[";
    bool first_segment = true;
    if (message.segments) {
        for (const prep::MessageSegment& segment : *message.segments) {
            if (!first_segment) out.push_back(',');
            first_segment = false;
            bool first = true;
            std::string object = "{";
            append_dump_member(object, first, "type");
            append_quoted(object, segment.type);
            append_dump_member(object, first, "content");
            append_quoted(object, segment.content);
            if (segment.tool_name) {
                append_dump_member(object, first, "toolName");
                append_quoted(object, *segment.tool_name);
            }
            if (segment.tool_args) {
                append_dump_member(object, first, "toolArgs");
                append_quoted(object, *segment.tool_args);
            }
            if (segment.tool_result) {
                append_dump_member(object, first, "toolResult");
                append_quoted(object, *segment.tool_result);
            }
            if (segment.tool_call_id) {
                append_dump_member(object, first, "toolCallId");
                append_quoted(object, *segment.tool_call_id);
            }
            if (segment.signature) {
                append_dump_member(object, first, "signature");
                append_quoted(object, *segment.signature);
            }
            if (segment.signature_provider) {
                append_dump_member(object, first, "signatureProvider");
                append_quoted(object, *segment.signature_provider);
            }
            if (segment.tool_transcription) {
                append_dump_member(object, first, "toolTranscription");
                append_quoted(object, *segment.tool_transcription);
            }
            if (segment.error_code) {
                append_dump_member(object, first, "errorCode");
                append_quoted(object, *segment.error_code);
            }
            append_dump_member(object, first, "responseOutputItems");
            object += std::to_string(segment.response_output_items.size());
            if (segment.response_output_item_provider) {
                append_dump_member(object, first, "responseOutputItemProvider");
                append_quoted(object, *segment.response_output_item_provider);
            }
            object.push_back('}');
            out += object;
        }
    }
    out.push_back(']');
    return out;
}

std::string dump_tool_call(const prep::ChatMessage& message) {
    if (!message.tool_call) return "-";
    const prep::ToolCallData& call = *message.tool_call;
    std::string object = "{";
    bool first = true;
    append_dump_member(object, first, "toolName");
    append_quoted(object, call.tool_name);
    append_dump_member(object, first, "arguments");
    append_quoted(object, call.arguments);
    append_dump_member(object, first, "result");
    append_quoted(object, call.result);
    if (call.signature) {
        append_dump_member(object, first, "signature");
        append_quoted(object, *call.signature);
    }
    if (call.tool_call_id) {
        append_dump_member(object, first, "toolCallId");
        append_quoted(object, *call.tool_call_id);
    }
    if (call.display_name) {
        append_dump_member(object, first, "displayName");
        append_quoted(object, *call.display_name);
    }
    if (call.result_text) {
        append_dump_member(object, first, "resultText");
        append_quoted(object, *call.result_text);
    }
    if (call.structured_result) {
        append_dump_member(object, first, "structuredResult");
        append_quoted(object, *call.structured_result);
    }
    if (call.transcription) {
        append_dump_member(object, first, "transcription");
        append_quoted(object, *call.transcription);
    }
    append_dump_member(object, first, "responseOutputItems");
    object += std::to_string(call.response_output_items.size());
    if (call.response_output_item_provider) {
        append_dump_member(object, first, "responseOutputItemProvider");
        append_quoted(object, *call.response_output_item_provider);
    }
    object.push_back('}');
    return object;
}

std::string dump_attachments(const prep::ChatMessage& message) {
    std::string out = "[";
    bool first_item = true;
    if (message.attachment_meta) {
        for (const prep::AttachmentItem& item : message.attachment_meta->items) {
            if (!first_item) out.push_back(',');
            first_item = false;
            std::string object = "{";
            bool first = true;
            append_dump_member(object, first, "type");
            append_quoted(object, item.type);
            if (item.image_index) {
                append_dump_member(object, first, "imageIndex");
                object += std::to_string(*item.image_index);
            }
            if (item.page_count) {
                append_dump_member(object, first, "pageCount");
                object += std::to_string(*item.page_count);
            }
            if (item.file_size) {
                append_dump_member(object, first, "fileSize");
                object += std::to_string(*item.file_size);
            }
            if (item.pixel_width) {
                append_dump_member(object, first, "pixelWidth");
                object += std::to_string(*item.pixel_width);
            }
            if (item.pixel_height) {
                append_dump_member(object, first, "pixelHeight");
                object += std::to_string(*item.pixel_height);
            }
            object.push_back('}');
            out += object;
        }
    }
    out.push_back(']');
    return out;
}

void append_message_lines(std::string& out, const std::vector<prep::ChatMessage>& messages) {
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const prep::ChatMessage& message = messages[index];
        std::string line = "msg|" + std::to_string(index) + "|" + esc(message.id) + "|" +
                           esc(message.parent_id ? *message.parent_id : "-") + "|" +
                           participant_name(message.participant) + "|" + status_name(message.status) +
                           "|" + esc(message.text) + "|" + esc([&] {
                               std::string joined;
                               for (std::size_t position = 0; position < message.images.size();
                                    ++position) {
                                   if (position > 0) joined += ',';
                                   joined += message.images[position];
                               }
                               return joined;
                           }()) +
                           "|" + esc(message.model_name ? *message.model_name : "-") + "|" +
                           esc(message.thoughts ? *message.thoughts : "-") + "|" +
                           std::to_string(message.timestamp) + "|" +
                           esc(message.run_id ? *message.run_id : "-") + "|" +
                           (message.run_sequence ? std::to_string(*message.run_sequence) : "-") +
                           "|" + esc(dump_segments(message)) + "|" + esc(dump_tool_call(message)) +
                           "|" + esc(dump_attachments(message)) + "\n";
        out += line;
    }
}

std::string join_ids(const std::vector<std::string>& ids) {
    std::string out;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index > 0) out += ',';
        out += ids[index];
    }
    return out;
}

std::vector<std::string> ids_of(const std::vector<prep::ChatMessage>& messages) {
    std::vector<std::string> ids;
    for (const prep::ChatMessage& message : messages) ids.push_back(message.id);
    return ids;
}

// -- scenario runner ---------------------------------------------------------------------------------

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string name = require_string(scenario, "name");
    if (name != stem) scenario_fail(stem, "scenario name '" + name + "' must match the file stem");
    const std::string mode = string_or(scenario, "mode", "prepare");

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
        std::vector<prep::ChatMessage> messages;
        if (const json::Value* raw_messages = field(scenario, "messages");
            raw_messages != nullptr && !is_json_null(raw_messages)) {
            if (raw_messages->kind != json::Kind::Array) scenario_fail(stem, "messages must be an array");
            for (const json::Value& entry : raw_messages->elements) {
                messages.push_back(build_message(entry));
            }
        }

        prep::ContextCostModel costs = prep::ContextCostModel::default_model();
        if (const auto model = optional_string(scenario, "costModel")) {
            costs = prep::cost_model_for(prep::parse_prefixed_model_id(*model), o200k_counter());
        }

        const int budget = int_or(scenario, "budget", 1000);
        const bool include_reasoning = bool_or(scenario, "includeAssistantReasoning", false);
        const int fixed_token_cost = int_or(scenario, "fixedTokenCost", 0);

        if (mode == "prepare") {
            append_message_lines(out, prep::prepare_messages(messages, budget, include_reasoning, costs));
        } else if (mode == "compactProjection") {
            append_message_lines(out, prep::apply_nearest_context_compact(messages));
        } else if (mode == "canonical") {
            append_message_lines(out, prep::canonical_context_messages(messages));
        } else if (mode == "strip") {
            append_message_lines(out, prep::strip_empty_turns(messages));
        } else if (mode == "merge") {
            append_message_lines(out, prep::merge_consecutive_same_role(messages));
        } else if (mode == "validateTools") {
            append_message_lines(out, prep::validate_tool_messages(messages));
        } else if (mode == "limit") {
            append_message_lines(out, prep::limit_context(messages, budget, include_reasoning, costs));
        } else if (mode == "units") {
            for (const auto& unit : prep::protocol_atomic_units(messages)) {
                line("unit", esc(join_ids(ids_of(unit))));
            }
        } else if (mode == "usage") {
            const prep::FixedContextComposition composition{
                int_or(scenario, "fixedSystemPromptTokens", 0),
                int_or(scenario, "fixedToolTokens", 0),
            };
            const prep::ContextWindowUsage usage = prep::context_window_usage(
                messages, budget, fixed_token_cost, include_reasoning, composition, costs);
            out += "usage|" + std::to_string(usage.estimated_token_count) + "|" +
                   std::to_string(usage.token_budget) + "|" +
                   std::to_string(usage.logical_message_count) + "|" +
                   (usage.has_compact_boundary ? "1" : "0") + "|" +
                   std::to_string(usage.system_prompt_tokens) + "|" +
                   std::to_string(usage.tool_tokens) + "|" +
                   std::to_string(usage.message_tokens()) + "|" +
                   openai::format_kotlin_float(usage.progress()) + "\n";
        } else if (mode == "retained") {
            line("retained", esc(join_ids(prep::context_window_retained_message_ids(
                                  messages, budget, fixed_token_cost, include_reasoning, costs))));
        } else if (mode == "splitCompact") {
            const prep::ContextRetentionSplit split = prep::split_context_for_compact_retention(
                messages, int_or(scenario, "retainMessages", 0));
            line("prefix", esc(join_ids(ids_of(split.prefix))));
            line("retained", esc(join_ids(ids_of(split.retained))));
            line("count", std::to_string(split.retained_message_count));
        } else if (mode == "splitLogical") {
            const prep::LogicalContextSplit split = prep::split_logical_context(
                messages, int_or(scenario, "retainLogicalMessages", 0));
            line("prefix", esc(join_ids(ids_of(split.prefix))));
            line("suffix", esc(join_ids(ids_of(split.suffix))));
            line("count", std::to_string(split.logical_message_count));
        } else if (mode == "estimate") {
            line("estimate", std::to_string(prep::CostModelContextEstimator(costs).estimate(
                                 messages, include_reasoning)));
        } else if (mode == "estimateFixed" || mode == "fixedComposition") {
            const std::optional<std::string> system_prompt = optional_string(scenario, "systemPrompt");
            const std::optional<std::string> initial_user_prompt =
                optional_string(scenario, "initialUserPrompt");
            const std::vector<openai::ToolDefinition> tools = build_tools(field(scenario, "tools"));
            const bool code_execution = bool_or(scenario, "codeExecutionEnabled", false);
            const bool google_search = bool_or(scenario, "googleSearchEnabled", false);
            const bool open_ai_web_search = bool_or(scenario, "openAiWebSearchEnabled", false);
            const prep::CostModelContextEstimator estimator(costs);
            if (mode == "estimateFixed") {
                line("fixed", std::to_string(estimator.estimate_fixed(
                                  system_prompt, tools, initial_user_prompt, code_execution,
                                  google_search, open_ai_web_search)));
            } else {
                const prep::FixedContextComposition composition = estimator.estimate_fixed_composition(
                    system_prompt, tools, initial_user_prompt, code_execution, google_search,
                    open_ai_web_search);
                line("fixedc", std::to_string(composition.system_prompt_tokens) + "|" +
                                   std::to_string(composition.tool_tokens));
            }
        } else if (mode == "estimateText") {
            line("text", std::to_string(prep::CostModelContextEstimator(costs).estimate_text(
                             string_or(scenario, "text", ""))));
        } else if (mode == "projectAssistantImages") {
            append_message_lines(out, prep::project_assistant_images_to_latest_user_message(
                                          messages, bool_or(scenario, "includeImages", true)));
        } else if (mode == "projectToolResultImages") {
            append_message_lines(out, prep::project_tool_result_images_to_user_message(
                                          messages, bool_or(scenario, "includeImages", true)));
        } else if (mode == "projectStatuses") {
            // The app's formatter is caller-injected; the stand pins the same fixed shape.
            append_message_lines(out, prep::project_generation_statuses_for_api(
                                          messages, [](const std::string& detail) {
                                              return "formatted:" + detail;
                                          }));
        } else {
            scenario_fail(stem, "unknown mode: " + mode);
        }
    } catch (const prep::PrepError& error) {
        out += "error|" + error.class_name + "\n";
    } catch (const agora::core::test::Failure&) {
        throw;
    } catch (const std::exception& error) {
        out += "error|";
        out += error.what();
        out += "\n";
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

TEST(message_preparation_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MESSAGE_PREP_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));

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
