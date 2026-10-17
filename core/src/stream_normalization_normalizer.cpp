// ProviderStreamNormalizer: provider-neutral compatibility parsing between wire decoders and
// executable authority (text-held tool calls, thinking projections, malformed stand-ins).

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/stream_normalization.hpp"
#include "stream_normalization_internal.hpp"

namespace agora::core::stream_normalization {

namespace json = agora::core::json;
namespace unicode = agora::core::unicode;

namespace {

bool starts_with_bytes(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string ascii_lowercased(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = detail::ascii_lower(c);
    return out;
}

std::string extension(const std::string& target, const std::string& previous) {
    if (starts_with_bytes(target, previous)) return target.substr(previous.size());
    return target;
}

/** Java String.compareTo order: lexicographic over UTF-16 code units. */
int compare_utf16_units(std::string_view lhs, std::string_view rhs) {
    struct Utf16Stream {
        std::string_view text;
        std::size_t index = 0;
        std::uint32_t pending_low = 0;
        bool has_pending = false;

        bool next(std::uint32_t& unit) {
            if (has_pending) {
                unit = pending_low;
                has_pending = false;
                return true;
            }
            if (index >= text.size()) return false;
            const auto decoded = unicode::decode_utf8(text.data() + index, text.size() - index);
            index += decoded.length;
            const std::uint32_t code_point = decoded.code_point;
            if (code_point >= 0x10000) {
                const std::uint32_t value = code_point - 0x10000;
                unit = 0xD800 + (value >> 10);
                pending_low = 0xDC00 + (value & 0x3FF);
                has_pending = true;
            } else {
                unit = code_point;
            }
            return true;
        }
    };
    Utf16Stream left{lhs};
    Utf16Stream right{rhs};
    while (true) {
        std::uint32_t left_unit = 0;
        std::uint32_t right_unit = 0;
        const bool has_left = left.next(left_unit);
        const bool has_right = right.next(right_unit);
        if (!has_left || !has_right) {
            if (has_left == has_right) return 0;
            return has_left ? 1 : -1;
        }
        if (left_unit != right_unit) return left_unit < right_unit ? -1 : 1;
    }
}

json::Value canonicalize_value(const json::Value& element) {
    if (element.kind == json::Kind::Object) {
        // ParsedJsonElement keeps the last duplicate value at the first position; then
        // canonicalize sorts the entries by key.
        std::vector<json::Member> unique;
        for (const auto& member : element.members) {
            bool replaced = false;
            for (auto& existing : unique) {
                if (existing.key == member.key) {
                    existing.value = member.value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) unique.push_back(member);
        }
        std::stable_sort(unique.begin(), unique.end(), [](const json::Member& a, const json::Member& b) {
            return compare_utf16_units(a.key, b.key) < 0;
        });
        json::Value object;
        object.kind = json::Kind::Object;
        object.members.reserve(unique.size());
        for (auto& member : unique) {
            json::Value value = canonicalize_value(member.value);
            object.members.push_back(json::Member{std::move(member.key), std::move(value)});
        }
        return object;
    }
    if (element.kind == json::Kind::Array) {
        json::Value array;
        array.kind = json::Kind::Array;
        array.elements.reserve(element.elements.size());
        for (const auto& child : element.elements) {
            array.elements.push_back(canonicalize_value(child));
        }
        return array;
    }
    return element;
}

const std::vector<std::string_view>& tagged_prefixes() {
    static const std::vector<std::string_view> kPrefixes = {
        "<" "tool_" "call>",
        "<" "invoke",
        "<" "antml:" "invoke",
    };
    return kPrefixes;
}

}  // namespace

ProviderStreamNormalizer::ProviderStreamNormalizer(
    std::vector<std::string> offered_tool_names,
    bool native_text_parsing_authoritative,
    UuidFn uuid)
    : offered_tool_names_(offered_tool_names.begin(), offered_tool_names.end()),
      structured_thinking_(true),
      uuid_(std::move(uuid)) {
    // Native authority suppresses generic text tool recovery only.
    if (!offered_tool_names_.empty() && !native_text_parsing_authoritative) {
        text_tool_parser_.emplace(uuid_);
    }
}

void ProviderStreamNormalizer::emit(const StreamEvent& event, const Downstream& downstream) {
    if (finished_) throw IllegalStateError("Provider stream normalizer already finished");
    if (const auto* text = std::get_if<TextChunk>(&event)) {
        flush_structured_thinking(downstream);
        route_provider_text(text->text, downstream);
        return;
    }
    if (const auto* thought = std::get_if<ThoughtChunk>(&event)) {
        flush_inline_thinking(downstream);
        route_structured_thought(*thought, downstream);
        return;
    }
    if (const auto* call = std::get_if<ToolCallRequest>(&event)) {
        flush_thinking(downstream);
        record_native_call(*call);
        downstream(event);
        return;
    }
    if (const auto* calls = std::get_if<ToolCallsRequest>(&event)) {
        flush_thinking(downstream);
        for (const ToolCallRequest& call : calls->calls) record_native_call(call);
        downstream(event);
        return;
    }
    if (std::holds_alternative<ErrorEvent>(event)) {
        finish(downstream, false);
        downstream(event);
        return;
    }
    flush_thinking(downstream);
    downstream(event);
}

void ProviderStreamNormalizer::finish(const Downstream& downstream, bool release_text_tools_flag) {
    if (finished_) return;
    flush_thinking(downstream);
    if (release_text_tools_flag) {
        finish_text_tools(downstream);
        release_text_tools(downstream);
    } else {
        flush_aborted_text(downstream);
    }
    finished_ = true;
}

void ProviderStreamNormalizer::route_provider_text(
    const std::string& content,
    const Downstream& downstream) {
    route_raw_text(content, downstream);
}

void ProviderStreamNormalizer::route_raw_text(
    const std::string& content,
    const Downstream& downstream) {
    inline_thinking_.feed(
        content,
        true,
        [&](const std::string& text) { route_tool_text(text, downstream); },
        [&](const std::string& thought) { emit_inline_thought(thought, downstream); });
}

void ProviderStreamNormalizer::route_structured_thought(
    const ThoughtChunk& event,
    const Downstream& downstream) {
    const bool was_in_thinking = structured_thinking_.in_thinking();
    structured_title_ = event.title;
    structured_signature_ = event.signature;
    if (event.thought.empty()) {
        if (was_in_thinking || event.title.has_value() || event.signature.has_value()) {
            emit_native_thought("", event.title, event.signature, downstream);
        }
        return;
    }

    bool emitted_thought = false;
    std::vector<std::string> pending_text;
    structured_thinking_.feed(
        event.thought,
        true,
        [&](const std::string& text) {
            if (!text.empty()) pending_text.push_back(text);
        },
        [&](const std::string& thought) {
            emitted_thought = true;
            emit_native_thought(thought, event.title, event.signature, downstream);
        });
    if (!emitted_thought && was_in_thinking &&
        (event.title.has_value() || event.signature.has_value())) {
        emit_native_thought("", event.title, event.signature, downstream);
    }
    for (const std::string& text : pending_text) route_provider_text(text, downstream);
}

void ProviderStreamNormalizer::flush_thinking(const Downstream& downstream) {
    flush_structured_thinking(downstream);
    flush_inline_thinking(downstream);
}

void ProviderStreamNormalizer::flush_structured_thinking(const Downstream& downstream) {
    structured_thinking_.flush(
        true,
        [&](const std::string& text) { route_provider_text(text, downstream); },
        [&](const std::string& thought) {
            emit_native_thought(thought, structured_title_, structured_signature_, downstream);
        });
}

void ProviderStreamNormalizer::flush_inline_thinking(const Downstream& downstream) {
    inline_thinking_.flush(
        true,
        [&](const std::string& text) { route_tool_text(text, downstream); },
        [&](const std::string& thought) { emit_inline_thought(thought, downstream); });
}

void ProviderStreamNormalizer::emit_inline_thought(
    const std::string& content,
    const Downstream& downstream) {
    if (content.empty()) return;
    inline_thought_ += content;
    const std::string& inline_value = inline_thought_;
    const std::string& native_value = native_thought_;
    std::string target;
    if (native_value.empty()) {
        target = inline_value;
    } else if (starts_with_bytes(native_value, inline_value)) {
        target.clear();
    } else if (starts_with_bytes(inline_value, native_value)) {
        target = inline_value.substr(native_value.size());
    } else {
        target = inline_value;
    }
    const std::string delta = extension(target, inline_visible_projection_);
    if (!delta.empty()) {
        downstream(StreamEvent{ThoughtChunk{delta, std::nullopt, std::nullopt}});
    }
    inline_visible_projection_ = target;
}

void ProviderStreamNormalizer::emit_native_thought(
    const std::string& content,
    const std::optional<std::string>& title,
    const std::optional<std::string>& signature,
    const Downstream& downstream) {
    if (!content.empty()) native_thought_ += content;
    const std::string& native_value = native_thought_;
    const std::string& inline_value = inline_thought_;
    std::string target;
    if (inline_value.empty()) {
        target = native_value;
    } else if (starts_with_bytes(inline_value, native_value)) {
        target.clear();
    } else if (starts_with_bytes(native_value, inline_value)) {
        target = native_value.substr(inline_value.size());
    } else {
        target = native_value;
    }
    const std::string delta = extension(target, native_visible_projection_);
    if (!delta.empty()) {
        downstream(StreamEvent{ThoughtChunk{delta, title, signature}});
    } else if (title.has_value() || signature.has_value()) {
        downstream(StreamEvent{ThoughtChunk{std::string{}, title, signature}});
    }
    native_visible_projection_ = target;
}

void ProviderStreamNormalizer::route_tool_text(
    const std::string& content,
    const Downstream& downstream) {
    if (!text_tool_parser_.has_value() || content.empty()) {
        if (!content.empty()) downstream(StreamEvent{TextChunk{content}});
        return;
    }
    switch (tool_probe_) {
        case ToolProbe::kText:
            downstream(StreamEvent{TextChunk{content}});
            return;
        case ToolProbe::kTagged:
            feed_tagged(content);
            return;
        case ToolProbe::kJson:
            tool_probe_buffer_ += content;
            return;
        case ToolProbe::kUndecided:
            tool_probe_buffer_ += content;
            decide_tool_probe(downstream);
            return;
    }
}

void ProviderStreamNormalizer::decide_tool_probe(const Downstream& downstream) {
    const std::string raw = tool_probe_buffer_;
    const std::string candidate = detail::trim_start(raw);
    if (candidate.empty()) return;
    if (candidate.front() == '{' || candidate.front() == '[') {
        tool_probe_ = ToolProbe::kJson;
        return;
    }
    const std::string lower = ascii_lowercased(candidate);
    for (const std::string_view prefix : tagged_prefixes()) {
        if (starts_with_bytes(lower, prefix)) {
            tool_probe_ = ToolProbe::kTagged;
            tool_probe_buffer_.clear();
            feed_tagged(raw);
            return;
        }
    }
    for (const std::string_view prefix : tagged_prefixes()) {
        if (starts_with_bytes(prefix, lower)) return;
    }
    tool_probe_ = ToolProbe::kText;
    tool_probe_buffer_.clear();
    downstream(StreamEvent{TextChunk{raw}});
}

void ProviderStreamNormalizer::feed_tagged(const std::string& content) {
    StreamingTextToolCallParser::Callbacks callbacks;
    callbacks.on_text = [this](const std::string& text) { tagged_residual_text_ += text; };
    callbacks.on_update = [](const StreamingTextToolCallParser::Snapshot&) {};
    callbacks.on_complete = [this](const StreamingTextToolCallParser::CompletedCall& call) {
        add_text_candidate(call);
    };
    callbacks.on_malformed = [this](const std::string& cause) { report_malformed(cause); };
    text_tool_parser_->feed(content, callbacks);
}

void ProviderStreamNormalizer::add_text_candidate(
    const StreamingTextToolCallParser::CompletedCall& call) {
    text_tool_candidates_.push_back(TextToolCandidate{
        call.stream_key,
        call.id,
        call.name,
        call.arguments,
    });
}

void ProviderStreamNormalizer::finish_text_tools(const Downstream& downstream) {
    switch (tool_probe_) {
        case ToolProbe::kUndecided:
        case ToolProbe::kText:
            if (!tool_probe_buffer_.empty()) {
                downstream(StreamEvent{TextChunk{tool_probe_buffer_}});
                tool_probe_buffer_.clear();
            }
            return;
        case ToolProbe::kJson:
            finish_json_tool(downstream);
            return;
        case ToolProbe::kTagged: {
            StreamingTextToolCallParser::Callbacks callbacks;
            callbacks.on_text = [this](const std::string& text) { tagged_residual_text_ += text; };
            callbacks.on_update = [](const StreamingTextToolCallParser::Snapshot&) {};
            callbacks.on_complete = [this](const StreamingTextToolCallParser::CompletedCall& call) {
                add_text_candidate(call);
            };
            callbacks.on_malformed = [this](const std::string& cause) { report_malformed(cause); };
            text_tool_parser_->flush(callbacks);
            if (!json::is_blank(tagged_residual_text_)) {
                report_malformed("Tagged tool payload contained ordinary text", tagged_residual_text_);
            }
            return;
        }
    }
}

void ProviderStreamNormalizer::finish_json_tool(const Downstream& downstream) {
    const std::string raw = tool_probe_buffer_;
    const std::vector<ParsedTextCall> parsed = parse_tool_call_text(raw);
    if (parsed.empty()) {
        if (looks_like_explicit_json_tool(raw)) {
            report_malformed("Whole-content tool payload was malformed", raw);
        } else {
            downstream(StreamEvent{TextChunk{raw}});
        }
        return;
    }
    for (const ParsedTextCall& call : parsed) {
        text_tool_candidates_.push_back(TextToolCandidate{
            "text_tool_" + (uuid_ ? uuid_() : random_uuid_v4()),
            call.id,
            call.name,
            call.arguments,
        });
    }
}

void ProviderStreamNormalizer::release_text_tools(const Downstream& downstream) {
    if (text_tool_candidates_.empty() && malformed_text_calls_.empty()) return;
    std::set<std::string> seen_ids = native_call_ids_;
    std::vector<ToolCallRequest> calls;
    for (const TextToolCandidate& candidate : text_tool_candidates_) {
        // Only identity problems, which make a result impossible to pair, stay fatal below.
        const std::optional<ToolFingerprint> fingerprint =
            this->fingerprint(candidate.name, candidate.arguments);
        const std::string id = candidate.id.has_value()
            ? *candidate.id
            : std::string("call_text_") + (uuid_ ? uuid_() : random_uuid_v4());
        if (!detail::is_safe_wire_tool_call_id(id)) {
            calls.push_back(malformed_tool_call_request(
                "Text tool payload contained an invalid or duplicate id",
                candidate.name,
                candidate.arguments,
                candidate.stream_key,
                std::nullopt,
                uuid_));
            continue;
        }
        if (candidate.id.has_value() && native_call_ids_.count(*candidate.id) != 0) {
            const auto recorded = native_fingerprints_by_id_.find(*candidate.id);
            if (recorded != native_fingerprints_by_id_.end() && recorded->second == fingerprint) {
                continue;
            }
            calls.push_back(malformed_tool_call_request(
                "Text tool payload contained an invalid or duplicate id",
                candidate.name,
                candidate.arguments,
                candidate.stream_key,
                std::nullopt,
                uuid_));
            continue;
        }
        if (fingerprint.has_value() &&
            native_fingerprints_.count({fingerprint->name, fingerprint->arguments}) != 0) {
            continue;
        }
        if (!seen_ids.insert(id).second) {
            calls.push_back(malformed_tool_call_request(
                "Text tool payload contained an invalid or duplicate id",
                candidate.name,
                candidate.arguments,
                candidate.stream_key,
                std::nullopt,
                uuid_));
            continue;
        }
        ToolCallRequest call;
        call.id = id;
        call.name = candidate.name;
        call.arguments = json::is_blank(candidate.arguments) ? "{}" : candidate.arguments;
        call.stream_key = candidate.stream_key;
        calls.push_back(std::move(call));
    }
    // Unparsable payloads are answered as malformed calls after the parsable ones.
    for (const ToolCallRequest& malformed : malformed_text_calls_) calls.push_back(malformed);
    malformed_text_calls_.clear();
    if (calls.size() == 1) downstream(StreamEvent{std::move(calls.front())});
    if (calls.size() > 1) downstream(StreamEvent{ToolCallsRequest{std::move(calls)}});
}

void ProviderStreamNormalizer::record_native_call(const ToolCallRequest& call) {
    native_call_ids_.insert(call.id);
    const std::optional<ToolFingerprint> fingerprint = this->fingerprint(call.name, call.arguments);
    native_fingerprints_by_id_[call.id] = fingerprint;
    if (fingerprint.has_value()) {
        native_fingerprints_.insert({fingerprint->name, fingerprint->arguments});
    }
}

std::optional<ProviderStreamNormalizer::ToolFingerprint> ProviderStreamNormalizer::fingerprint(
    const std::string& name,
    const std::string& arguments) const {
    const std::string source = json::is_blank(arguments) ? "{}" : arguments;
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return std::nullopt;
    const json::Value canonical = canonicalize_value(*parsed.value);
    return ToolFingerprint{name, json::to_kotlin_string(canonical)};
}

void ProviderStreamNormalizer::report_malformed(
    const std::string& cause,
    const std::optional<std::string>& raw) {
    if (malformed_text_tool_) return;
    malformed_text_tool_ = true;
    malformed_text_calls_.push_back(malformed_tool_call_request(
        cause,
        std::nullopt,
        raw,
        std::nullopt,
        std::nullopt,
        uuid_));
}

void ProviderStreamNormalizer::flush_aborted_text(const Downstream& downstream) {
    if ((tool_probe_ == ToolProbe::kUndecided || tool_probe_ == ToolProbe::kJson) &&
        !tool_probe_buffer_.empty()) {
        downstream(StreamEvent{TextChunk{tool_probe_buffer_}});
        tool_probe_buffer_.clear();
    }
}

bool ProviderStreamNormalizer::looks_like_explicit_json_tool(const std::string& content) {
    const std::string compact = detail::trim_start(content);
    return compact.find("\"name\"") != std::string::npos ||
        compact.find("\"function\"") != std::string::npos ||
        compact.find("\"arguments\"") != std::string::npos ||
        compact.find("\"parameters\"") != std::string::npos;
}

}  // namespace agora::core::stream_normalization
