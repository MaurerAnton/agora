// Golden parity test for model/RunRecoveryPolicy.kt.
//
// The traces are produced by compiling the real Kotlin policy against the real MessageSegment,
// Participant and MessageStatus declarations extracted verbatim from ChatMessage.kt. They pin the
// complete data-class toString() of the before/after segment lists, so untouched fields are part
// of the contract too, plus every raw-JSON outcome (null, verbatim raw or compact re-encoding).
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/run_recovery.hpp"
#include "check.hpp"

namespace {

namespace recovery = agora::core::run_recovery;
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

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

const Value& require(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string("missing scenario field: ") + std::string(name));
    return *value;
}

std::string required_text(const Value& object, std::string_view name) {
    const Value& value = require(object, name);
    if (value.kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value.text;
}

std::string text_or_empty(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::optional<std::string> optional_text(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind == Kind::Literal) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

int int_value(const Value& object, std::string_view name, int fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be an int");
    try {
        return std::stoi(value->text);
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " is not an int");
    }
}

std::int64_t long_value(const Value& object, std::string_view name, std::int64_t fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a long");
    try {
        return std::stoll(value->text);
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " is not a long");
    }
}

const std::vector<Value>& array_or_empty(const Value& object, std::string_view name) {
    static const std::vector<Value> empty;
    const Value* value = field(object, name);
    if (value == nullptr) return empty;
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    return value->elements;
}

recovery::ToolImageAttachment parse_image(const Value& value) {
    recovery::ToolImageAttachment image;
    image.path = required_text(value, "path");
    image.mime_type = required_text(value, "mimeType");
    image.size_bytes = long_value(value, "sizeBytes", 0);
    const Value* width = field(value, "width");
    if (width != nullptr && width->kind == Kind::Literal && width->text != "null") {
        image.width = int_value(value, "width", 0);
    }
    const Value* height = field(value, "height");
    if (height != nullptr && height->kind == Kind::Literal && height->text != "null") {
        image.height = int_value(value, "height", 0);
    }
    image.sha256 = required_text(value, "sha256");
    return image;
}

recovery::StreamingTextDelta parse_delta(const Value& value) {
    recovery::StreamingTextDelta delta;
    delta.sequence = long_value(value, "sequence", 0);
    delta.code_point_count = int_value(value, "codePointCount", 0);
    return delta;
}

recovery::MessageSegment parse_segment(const Value& value) {
    recovery::MessageSegment segment;
    segment.type = required_text(value, "type");
    segment.content = text_or_empty(value, "content");
    segment.tool_name = optional_text(value, "toolName");
    segment.tool_args = optional_text(value, "toolArgs");
    segment.tool_result = optional_text(value, "toolResult");
    segment.tool_call_id = optional_text(value, "toolCallId");
    segment.signature = optional_text(value, "signature");
    segment.signature_provider = optional_text(value, "signatureProvider");
    const Value* duration = field(value, "durationMs");
    if (duration != nullptr && duration->kind == Kind::Literal && duration->text != "null") {
        segment.duration_ms = long_value(value, "durationMs", 0);
    }
    segment.tool_state = optional_text(value, "toolState");
    segment.tool_progress = optional_text(value, "toolProgress");
    segment.tool_target = optional_text(value, "toolTarget");
    segment.tool_display_name = optional_text(value, "toolDisplayName");
    segment.tool_result_text = optional_text(value, "toolResultText");
    segment.tool_structured_result = optional_text(value, "toolStructuredResult");
    for (const Value& image : array_or_empty(value, "toolImages")) {
        if (image.kind != Kind::Object) scenario_fail("toolImages entries must be objects");
        segment.tool_images.push_back(parse_image(image));
    }
    segment.tool_image_request_key = optional_text(value, "toolImageRequestKey");
    segment.tool_transcription = optional_text(value, "toolTranscription");
    for (const Value& item : array_or_empty(value, "responseOutputItems")) {
        if (item.kind != Kind::Object) scenario_fail("responseOutputItems entries must be objects");
        segment.response_output_items.push_back(json::to_kotlin_string(item));
    }
    segment.response_output_item_provider = optional_text(value, "responseOutputItemProvider");
    segment.error_code = optional_text(value, "errorCode");
    for (const Value& delta : array_or_empty(value, "streamingTextDeltas")) {
        if (delta.kind != Kind::Object) scenario_fail("streamingTextDeltas entries must be objects");
        segment.streaming_text_deltas.push_back(parse_delta(delta));
    }
    return segment;
}

std::string optional_string(const std::optional<std::string>& value) {
    return value.has_value() ? *value : "null";
}

std::string optional_number(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

std::string image_string(const recovery::ToolImageAttachment& image) {
    std::ostringstream out;
    out << "ToolImageAttachment(path=" << image.path
        << ", mimeType=" << image.mime_type
        << ", sizeBytes=" << image.size_bytes
        << ", width=" << (image.width.has_value() ? std::to_string(*image.width) : "null")
        << ", height=" << (image.height.has_value() ? std::to_string(*image.height) : "null")
        << ", sha256=" << image.sha256 << ')';
    return out.str();
}

std::string delta_string(const recovery::StreamingTextDelta& delta) {
    return "StreamingTextDelta(sequence=" + std::to_string(delta.sequence) +
           ", codePointCount=" + std::to_string(delta.code_point_count) + ')';
}

template <typename T, typename Fn>
std::string list_string(const std::vector<T>& values, Fn render) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ", ";
        out += render(values[i]);
    }
    out += ']';
    return out;
}

std::string segment_string(const recovery::MessageSegment& segment) {
    std::ostringstream out;
    out << "MessageSegment(type=" << segment.type
        << ", content=" << segment.content
        << ", toolName=" << optional_string(segment.tool_name)
        << ", toolArgs=" << optional_string(segment.tool_args)
        << ", toolResult=" << optional_string(segment.tool_result)
        << ", toolCallId=" << optional_string(segment.tool_call_id)
        << ", signature=" << optional_string(segment.signature)
        << ", signatureProvider=" << optional_string(segment.signature_provider)
        << ", durationMs=" << optional_number(segment.duration_ms)
        << ", toolState=" << optional_string(segment.tool_state)
        << ", toolProgress=" << optional_string(segment.tool_progress)
        << ", toolTarget=" << optional_string(segment.tool_target)
        << ", toolDisplayName=" << optional_string(segment.tool_display_name)
        << ", toolResultText=" << optional_string(segment.tool_result_text)
        << ", toolStructuredResult=" << optional_string(segment.tool_structured_result)
        << ", toolImages=" << list_string(segment.tool_images, image_string)
        << ", toolImageRequestKey=" << optional_string(segment.tool_image_request_key)
        << ", toolTranscription=" << optional_string(segment.tool_transcription)
        << ", responseOutputItems=[";
    for (std::size_t i = 0; i < segment.response_output_items.size(); ++i) {
        if (i != 0) out << ", ";
        out << segment.response_output_items[i];
    }
    out << ']'
        << ", responseOutputItemProvider=" << optional_string(segment.response_output_item_provider)
        << ", errorCode=" << optional_string(segment.error_code)
        << ", streamingTextDeltas="
        << list_string(segment.streaming_text_deltas, delta_string) << ')';
    return out.str();
}

std::string segments_string(const std::vector<recovery::MessageSegment>& segments) {
    return list_string(segments, segment_string);
}

recovery::Participant parse_participant(const std::string& value) {
    if (value == "USER") return recovery::Participant::user;
    if (value == "MODEL") return recovery::Participant::model;
    if (value == "ERROR") return recovery::Participant::error;
    scenario_fail("unknown participant " + value);
}

std::string participant_text(recovery::Participant value) {
    switch (value) {
        case recovery::Participant::user: return "USER";
        case recovery::Participant::model: return "MODEL";
        case recovery::Participant::error: return "ERROR";
    }
    scenario_fail("unknown participant");
}

recovery::MessageStatus parse_status(const std::string& value) {
    if (value == "TRANSCRIBING") return recovery::MessageStatus::transcribing;
    if (value == "SENDING") return recovery::MessageStatus::sending;
    if (value == "THINKING") return recovery::MessageStatus::thinking;
    if (value == "TOOL_CALLING") return recovery::MessageStatus::tool_calling;
    if (value == "SUCCESS") return recovery::MessageStatus::success;
    if (value == "STOPPED") return recovery::MessageStatus::stopped;
    if (value == "ERROR") return recovery::MessageStatus::error;
    scenario_fail("unknown status " + value);
}

std::string status_text(recovery::MessageStatus value) {
    switch (value) {
        case recovery::MessageStatus::transcribing: return "TRANSCRIBING";
        case recovery::MessageStatus::sending: return "SENDING";
        case recovery::MessageStatus::thinking: return "THINKING";
        case recovery::MessageStatus::tool_calling: return "TOOL_CALLING";
        case recovery::MessageStatus::success: return "SUCCESS";
        case recovery::MessageStatus::stopped: return "STOPPED";
        case recovery::MessageStatus::error: return "ERROR";
    }
    scenario_fail("unknown status");
}

std::string render_trace(const Value& scenario) {
    const std::string name = required_text(scenario, "name");
    const std::string fn = required_text(scenario, "fn");

    std::string out = "name|" + esc(name) + "\nfn|" + esc(fn) + '\n';
    if (fn == "recoverMessageStatus") {
        const recovery::Participant participant =
            parse_participant(required_text(scenario, "participant"));
        const Value& statuses = require(scenario, "statuses");
        if (statuses.kind != Kind::Array) scenario_fail("statuses must be an array");
        for (const Value& status : statuses.elements) {
            if (status.kind != Kind::String) scenario_fail("statuses must be strings");
            const recovery::MessageStatus input = parse_status(status.text);
            out += "args|" + participant_text(participant) + '|' + status_text(input) + '\n';
            out += "result|" +
                   status_text(recovery::recover_message_status(participant, input)) + '\n';
        }
    } else if (fn == "stopIncompleteTools") {
        std::vector<recovery::MessageSegment> segments;
        for (const Value& value : array_or_empty(scenario, "segments")) {
            segments.push_back(parse_segment(value));
        }
        out += "args|segments|" + esc(segments_string(segments)) + '\n';
        out += "result|segments|" +
               esc(segments_string(recovery::stop_incomplete_tools(segments))) + '\n';
    } else if (fn == "stopIncompleteToolsJson") {
        const std::string raw = required_text(scenario, "raw");
        const std::optional<std::string> result = recovery::stop_incomplete_tools_json(raw);
        out += "args|raw|" + esc(raw) + '\n';
        out += "result|string|" + (result.has_value() ? esc(*result) : std::string("-")) + '\n';
    } else {
        scenario_fail("unknown scenario function " + fn);
    }
    return out;
}

void check_scenario(const std::filesystem::path& scenario_path,
                    const std::filesystem::path& golden_dir) {
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value.has_value() || parsed.value->kind != Kind::Object) {
        throw agora::core::test::Failure("scenario must be a JSON object: " +
                                         scenario_path.filename().string());
    }
    const Value& scenario = *parsed.value;
    const std::string name = required_text(scenario, "name");
    const std::filesystem::path trace_path = golden_dir / (name + ".trace");
    if (!std::filesystem::is_regular_file(trace_path)) {
        throw agora::core::test::Failure("missing golden trace: " + trace_path.string());
    }
    const std::string expected = read_file(trace_path);
    const std::string actual = render_trace(scenario);
    if (actual != expected) {
        std::size_t offset = 0;
        while (offset < actual.size() && offset < expected.size() &&
               actual[offset] == expected[offset]) {
            ++offset;
        }
        throw agora::core::test::Failure(
            name + ": trace differs at byte " + std::to_string(offset) + " (actual " +
            std::to_string(actual.size()) + " bytes, expected " + std::to_string(expected.size()) +
            ")\nactual:   " + actual.substr(offset, 240) + "\nexpected: " +
            expected.substr(offset, 240));
    }
}

}  // namespace

TEST(run_recovery_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_RUN_RECOVERY_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(scenarios.size() >= 54);
    for (const std::filesystem::path& scenario : scenarios) check_scenario(scenario, golden_dir);
}
