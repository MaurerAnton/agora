// Golden parity test for the generation streaming segments (live timeline, merge, citation rebase,
// bounded output transform and the final snapshot).
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/citations.hpp"
#include "agora/core/generation_streaming_segments.hpp"
#include "agora/core/json.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/run_recovery.hpp"
#include "check.hpp"

namespace {

namespace streaming = agora::core::generation_streaming;
namespace prep = agora::core::prep;
namespace citations = agora::core::citations;
namespace run_recovery = agora::core::run_recovery;
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

std::string participant_name(prep::Participant participant) {
    switch (participant) {
        case prep::Participant::User: return "USER";
        case prep::Participant::Model: return "MODEL";
        case prep::Participant::Error: return "ERROR";
    }
    return "USER";
}

const Value& member_of(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

void append_optional(std::string& out, const std::optional<std::string>& value) {
    if (value.has_value()) {
        append_quoted(out, *value);
    } else {
        out += "null";
    }
}

prep::MessageStatus parse_status(const std::string& name) {
    if (name == "ERROR") return prep::MessageStatus::Error;
    if (name == "STOPPED") return prep::MessageStatus::Stopped;
    if (name == "THINKING") return prep::MessageStatus::Thinking;
    return prep::MessageStatus::Success;
}

prep::Participant parse_participant(const std::string& name) {
    if (name == "MODEL") return prep::Participant::Model;
    if (name == "ERROR") return prep::Participant::Error;
    return prep::Participant::User;
}

prep::MessageSegment build_segment(const Value& spec) {
    prep::MessageSegment segment;
    segment.type = required_string(spec, "type");
    segment.content = optional_string(spec, "content").value_or("");
    segment.signature = optional_string(spec, "signature");
    segment.signature_provider = optional_string(spec, "signatureProvider");
    if (const Value* duration = field(spec, "durationMs"); duration != nullptr) {
        segment.duration_ms = std::stoll(duration->text);
    }
    segment.error_code = optional_string(spec, "errorCode");
    if (const Value* deltas = field(spec, "streamingTextDeltas");
        deltas != nullptr && deltas->kind == Kind::Array) {
        for (const Value& delta : deltas->elements) {
            segment.streaming_text_deltas.push_back(run_recovery::StreamingTextDelta{
                std::stoll(member_of(delta, "sequence").text),
                std::stoi(member_of(delta, "codePointCount").text),
            });
        }
    }
    return segment;
}

prep::ChatMessage build_message(const Value& spec) {
    prep::ChatMessage message;
    message.id = required_string(spec, "id");
    message.parent_id = optional_string(spec, "parentId");
    message.text = optional_string(spec, "text").value_or("");
    message.status = parse_status(optional_string(spec, "status").value_or("SUCCESS"));
    message.participant = parse_participant(optional_string(spec, "participant").value_or("USER"));
    message.timestamp = field(spec, "timestamp") != nullptr
                            ? std::stoll(field(spec, "timestamp")->text)
                            : 0;
    if (const Value* segments = field(spec, "segments");
        segments != nullptr && segments->kind == Kind::Array) {
        std::vector<prep::MessageSegment> parsed;
        for (const Value& element : segments->elements) {
            parsed.push_back(build_segment(element));
        }
        message.segments = std::move(parsed);
    }
    return message;
}

std::string render_segment(const prep::MessageSegment& segment) {
    std::string out = "{\"type\":";
    append_quoted(out, segment.type);
    out += ",\"content\":";
    append_quoted(out, segment.content);
    out += ",\"signature\":";
    append_optional(out, segment.signature);
    out += ",\"signatureProvider\":";
    append_optional(out, segment.signature_provider);
    out += ",\"durationMs\":";
    if (segment.duration_ms.has_value()) {
        out += std::to_string(*segment.duration_ms);
    } else {
        out += "null";
    }
    out += ",\"errorCode\":";
    append_optional(out, segment.error_code);
    out += ",\"deltas\":[";
    for (std::size_t i = 0; i < segment.streaming_text_deltas.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += "{\"sequence\":" +
               std::to_string(segment.streaming_text_deltas[i].sequence) + ",\"codePointCount\":" +
               std::to_string(segment.streaming_text_deltas[i].code_point_count) + "}";
    }
    out += "]}";
    return out;
}

std::string render_segments(const std::optional<std::vector<prep::MessageSegment>>& segments) {
    std::string out = "[";
    if (segments.has_value()) {
        for (std::size_t i = 0; i < segments->size(); ++i) {
            if (i != 0) out.push_back(',');
            out += render_segment((*segments)[i]);
        }
    }
    out.push_back(']');
    return out;
}

std::string render_citation(const citations::CitationRecord& citation) {
    std::string out = "{\"sourceId\":";
    append_quoted(out, citation.source_id);
    out += ",\"provider\":";
    append_quoted(out, citation.provider);
    out += ",\"kind\":";
    append_quoted(out, citation.kind);
    out += ",\"title\":";
    append_quoted(out, citation.title);
    out += ",\"url\":";
    append_optional(out, citation.url);
    out += ",\"anchors\":[";
    for (std::size_t i = 0; i < citation.anchors.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += "{\"startIndex\":" + std::to_string(citation.anchors[i].start_index) +
               ",\"endIndex\":" + std::to_string(citation.anchors[i].end_index) +
               ",\"citedText\":";
        append_quoted(out, citation.anchors[i].cited_text);
        out.push_back('}');
    }
    out += "]}";
    return out;
}

citations::CitationRecord build_citation(const Value& spec) {
    citations::CitationRecord citation;
    citation.source_id = required_string(spec, "sourceId");
    citation.provider = optional_string(spec, "provider").value_or("openai");
    citation.kind = optional_string(spec, "kind").value_or("url");
    citation.title = optional_string(spec, "title").value_or("");
    citation.url = optional_string(spec, "url");
    if (const Value* anchors = field(spec, "anchors");
        anchors != nullptr && anchors->kind == Kind::Array) {
        for (const Value& anchor : anchors->elements) {
            citations::CitationAnchor parsed;
            parsed.start_index = std::stoi(member_of(anchor, "startIndex").text);
            parsed.end_index = std::stoi(member_of(anchor, "endIndex").text);
            parsed.cited_text = optional_string(anchor, "citedText").value_or("");
            citation.anchors.push_back(std::move(parsed));
        }
    }
    return citation;
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;

    std::string trace = "{";

    // --- thought timing ------------------------------------------------------------
    {
        std::vector<std::int64_t> clock;
        if (const Value* list = field(root, "clock"); list != nullptr) {
            for (const Value& element : list->elements) clock.push_back(std::stoll(element.text));
        }
        std::size_t clock_index = 0;
        streaming::ThoughtTiming timing([&]() -> std::int64_t {
            const std::int64_t value =
                clock_index < clock.size() ? clock[clock_index] : (clock.empty() ? 0 : clock.back());
            ++clock_index;
            return value;
        });
        trace += "\"timing\":[";
        bool first = true;
        if (const Value* steps = field(root, "timingSteps");
            steps != nullptr && steps->kind == Kind::Array) {
            for (const Value& step : steps->elements) {
                const std::string action = required_string(step, "action");
                if (!first) trace.push_back(',');
                first = false;
                if (action == "ensureStarted") {
                    timing.ensure_started();
                } else if (action == "finish") {
                    timing.finish_current();
                } else if (action == "reset") {
                    timing.reset_current_duration();
                } else if (action == "adopt") {
                    const Value* duration = field(step, "duration");
                    timing.adopt_total_duration(
                        duration != nullptr && duration->kind == Kind::Literal &&
                                duration->text != "null"
                            ? std::optional<std::int64_t>(std::stoll(duration->text))
                            : std::nullopt);
                }
                trace += "{\"action\":";
                append_quoted(trace, action);
                trace += ",\"current\":" + std::to_string(timing.current_duration_ms());
                trace += ",\"total\":";
                if (timing.total_duration_ms().has_value()) {
                    trace += std::to_string(*timing.total_duration_ms());
                } else {
                    trace += "null";
                }
                trace += ",\"live\":";
                const std::optional<std::int64_t> live_duration = timing.live_duration_ms();
                if (live_duration.has_value()) {
                    trace += std::to_string(*live_duration);
                } else {
                    trace += "null";
                }
                trace.push_back('}');
            }
        }
        trace += "]";
    }

    // --- status after the thought phase ---------------------------------------------
    {
        trace += ",\"statusAfterThought\":[";
        bool first = true;
        for (prep::MessageStatus status :
             {prep::MessageStatus::Thinking, prep::MessageStatus::Sending,
              prep::MessageStatus::Success, prep::MessageStatus::Error,
              prep::MessageStatus::Stopped}) {
            if (!first) trace.push_back(',');
            first = false;
            trace += "{\"status\":";
            append_quoted(trace, status_name(status));
            trace += ",\"after\":";
            append_quoted(trace, status_name(
                streaming::status_after_thought_phase_finished(status)));
            trace.push_back('}');
        }
        trace += "]";
    }

    // --- segment merge ---------------------------------------------------------------
    {
        std::vector<prep::MessageSegment> merged;
        trace += ",\"merged\":[";
        bool first = true;
        if (const Value* steps = field(root, "merge"); steps != nullptr && steps->kind == Kind::Array) {
            for (const Value& step : steps->elements) {
                streaming::append_merged_segment(&merged, build_segment(step));
                if (!first) trace.push_back(',');
                first = false;
                trace += render_segment(merged.back());
            }
        }
        trace += "]";
    }

    // --- live segments ---------------------------------------------------------------
    if (const Value* live = field(root, "live"); live != nullptr) {
        std::vector<prep::MessageSegment> flushed;
        if (const Value* list = field(*live, "flushed");
            list != nullptr && list->kind == Kind::Array) {
            for (const Value& element : list->elements) flushed.push_back(build_segment(element));
        }
        std::vector<run_recovery::StreamingTextDelta> deltas;
        if (const Value* list = field(*live, "answerDeltas");
            list != nullptr && list->kind == Kind::Array) {
            for (const Value& element : list->elements) {
                deltas.push_back(run_recovery::StreamingTextDelta{
                    std::stoll(member_of(element, "sequence").text),
                    std::stoi(member_of(element, "codePointCount").text),
                });
            }
        }
        const std::optional<std::vector<prep::MessageSegment>> segments = streaming::build_live_segments(
            flushed, optional_string(*live, "answer").value_or(""),
            optional_string(*live, "thought").value_or(""), optional_string(*live, "signature"),
            optional_string(*live, "signatureProvider"),
            field(*live, "thoughtDurationMs") != nullptr &&
                    field(*live, "thoughtDurationMs")->kind == Kind::Literal &&
                    field(*live, "thoughtDurationMs")->text != "null"
                ? std::optional<std::int64_t>(std::stoll(field(*live, "thoughtDurationMs")->text))
                : std::nullopt,
            optional_string(*live, "errorMessage"), optional_string(*live, "errorCode"), deltas);
        trace += ",\"liveSegments\":" + render_segments(segments);
    }

    // --- citation rebase -------------------------------------------------------------
    if (const Value* citation_spec = field(root, "citation"); citation_spec != nullptr) {
        const citations::CitationRecord citation = build_citation(*citation_spec);
        const std::int32_t provider_start =
            field(root, "providerAnswerStart") != nullptr
                ? std::stoi(field(root, "providerAnswerStart")->text)
                : 0;
        const std::string final_answer = optional_string(root, "finalAnswer").value_or("");
        trace += ",\"rebased\":" +
                 render_citation(streaming::rebase_citation_for_final_answer(
                     citation, provider_start, final_answer));
    }

    // --- bounded output transform ------------------------------------------------------
    if (const Value* message_spec = field(root, "message"); message_spec != nullptr) {
        const prep::ChatMessage message = build_message(*message_spec);
        const std::string replacement =
            field(root, "transform") != nullptr
                ? optional_string(*field(root, "transform"), "replacement").value_or("")
                : std::string();
        const prep::ChatMessage transformed = streaming::with_bounded_output_text_transform(
            message, [&replacement](const std::string&, prep::MessageStatus) { return replacement; });
        trace += ",\"bounded\":{\"text\":";
        append_quoted(trace, transformed.text);
        trace += ",\"segments\":" + render_segments(transformed.segments);
        trace += "}";
    }

    // --- terminal error ----------------------------------------------------------------
    if (const Value* spec = field(root, "terminalError"); spec != nullptr) {
        const std::optional<std::string> error = streaming::terminal_generation_error_message(
            parse_status(optional_string(*spec, "status").value_or("SUCCESS")),
            optional_string(*spec, "currentError"),
            optional_string(*spec, "fallbackError").value_or(""));
        trace += ",\"terminalError\":";
        if (error.has_value()) {
            append_quoted(trace, *error);
        } else {
            trace += "null";
        }
    }

    // --- final snapshot ------------------------------------------------------------------
    if (const Value* spec = field(root, "snapshot"); spec != nullptr) {
        streaming::FinalSnapshot snapshot;
        snapshot.message_id = required_string(*spec, "messageId");
        snapshot.parent_id = optional_string(*spec, "parentId");
        snapshot.text = optional_string(*spec, "text").value_or("");
        if (const Value* images = field(*spec, "images");
            images != nullptr && images->kind == Kind::Array) {
            for (const Value& element : images->elements) snapshot.images.push_back(element.text);
        }
        snapshot.thoughts = optional_string(*spec, "thoughts").value_or("");
        snapshot.thought_title = optional_string(*spec, "thoughtTitle");
        snapshot.token_count = field(*spec, "tokenCount") != nullptr
                                   ? std::stoi(field(*spec, "tokenCount")->text)
                                   : 0;
        snapshot.timestamp = field(*spec, "timestamp") != nullptr &&
                                    field(*spec, "timestamp")->kind == Kind::Literal &&
                                    field(*spec, "timestamp")->text != "null"
                                 ? std::stoll(field(*spec, "timestamp")->text)
                                 : 0;
        if (const Value* thought_time = field(*spec, "thoughtTimeMs");
            thought_time != nullptr && thought_time->kind == Kind::Literal &&
            thought_time->text != "null") {
            snapshot.thought_time_ms = std::stoll(thought_time->text);
        }
        snapshot.model_name = optional_string(*spec, "modelName").value_or("");
        if (const Value* flushed = field(*spec, "flushedSegments");
            flushed != nullptr && flushed->kind == Kind::Array) {
            for (const Value& element : flushed->elements) {
                snapshot.flushed_segments.push_back(build_segment(element));
            }
        }
        snapshot.answer_buffer = optional_string(*spec, "answerBuffer").value_or("");
        snapshot.thought_buffer = optional_string(*spec, "thoughtBuffer").value_or("");
        snapshot.thought_signature = optional_string(*spec, "thoughtSignature");
        snapshot.thought_signature_provider = optional_string(*spec, "thoughtSignatureProvider");
        if (const Value* thought_duration = field(*spec, "thoughtDurationMs");
            thought_duration != nullptr && thought_duration->kind == Kind::Literal &&
            thought_duration->text != "null") {
            snapshot.thought_duration_ms = std::stoll(thought_duration->text);
        }
        snapshot.error_message = optional_string(*spec, "errorMessage");
        snapshot.error_code = optional_string(*spec, "errorCode");
        snapshot.run_id = required_string(*spec, "runId");
        snapshot.run_sequence = std::stoll(field(*spec, "runSequence")->text);
        if (const Value* deltas = field(*spec, "answerDeltas");
            deltas != nullptr && deltas->kind == Kind::Array) {
            for (const Value& element : deltas->elements) {
                snapshot.answer_deltas.push_back(run_recovery::StreamingTextDelta{
                    std::stoll(member_of(element, "sequence").text),
                    std::stoi(member_of(element, "codePointCount").text),
                });
            }
        }
        const prep::ChatMessage message = streaming::to_message(snapshot);
        trace += ",\"snapshot\":{\"id\":";
        append_quoted(trace, message.id);
        trace += ",\"parentId\":";
        append_optional(trace, message.parent_id);
        trace += ",\"text\":";
        append_quoted(trace, message.text);
        trace += ",\"images\":[";
        for (std::size_t i = 0; i < message.images.size(); ++i) {
            if (i != 0) trace.push_back(',');
            append_quoted(trace, message.images[i]);
        }
        trace += "],\"thoughts\":";
        append_optional(trace, message.thoughts);
        trace += ",\"thoughtTitle\":";
        append_optional(trace, message.thought_title);
        trace += ",\"tokenCount\":" + std::to_string(message.token_count);
        trace += ",\"status\":";
        append_quoted(trace, status_name(message.status));
        trace += ",\"participant\":";
        append_quoted(trace, participant_name(message.participant));
        trace += ",\"timestamp\":" + std::to_string(message.timestamp);
        trace += ",\"thoughtTimeMs\":";
        if (message.thought_time_ms.has_value()) {
            trace += std::to_string(*message.thought_time_ms);
        } else {
            trace += "null";
        }
        trace += ",\"modelName\":";
        append_optional(trace, message.model_name);
        trace += ",\"runId\":";
        append_optional(trace, message.run_id);
        trace += ",\"runSequence\":";
        if (message.run_sequence.has_value()) {
            trace += std::to_string(*message.run_sequence);
        } else {
            trace += "null";
        }
        trace += ",\"segments\":" + render_segments(message.segments);
        trace += "}";
    }

    trace.push_back('}');
    return trace;
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

TEST(generation_streaming_segments_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_GENERATION_STREAMING_SEGMENTS_DIR;
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
