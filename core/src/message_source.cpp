// Port of model/MessageSource.kt.
//
// The Kotlin object delegates the wire format to kotlinx.serialization with
// `ignoreUnknownKeys = true` and `encodeDefaults = false`. The port reuses the core JSON layer,
// which already mirrors the kotlinx lexer (raw control bytes inside strings, BOM rejection,
// trailing-content rejection) and the kotlinx escaping/compact writer (lowercase \u00xx), so the
// canonical stored form matches byte for byte.
#include "agora/core/message_source.hpp"

#include <string_view>
#include <utility>

#include "agora/core/json.hpp"

namespace agora::core::message_source {
namespace {

const json::Value* last_member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

/**
 * A duplicated key is read last-wins, matching the streaming decoder's assignment order.
 * (kotlinx does not pin duplicate handling for this codec, so no scenario relies on it.)
 */
bool is_json_null(const json::Value& value) {
    return value.kind == json::Kind::Literal && value.text == "null";
}

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Task:
            return "task";
        case Kind::Loop:
            return "loop";
        case Kind::AskUser:
            return "ask_user";
        case Kind::Subagent:
            return "subagent";
    }
    return "task";
}

void append_string(std::string& out, std::string_view text) {
    out += '"';
    json::escape_to(out, text);
    out += '"';
}

}  // namespace

std::optional<Source> decode(const std::optional<std::string>& raw) {
    if (!raw.has_value() || json::is_blank(*raw)) return std::nullopt;

    const json::ParseResult parsed = json::parse(*raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return std::nullopt;
    const json::Value& root = *parsed.value;

    const json::Value* kind_value = last_member(root, "kind");
    if (kind_value == nullptr || kind_value->kind != json::Kind::String) return std::nullopt;

    Kind kind = Kind::Task;
    if (kind_value->text == "task") {
        kind = Kind::Task;
    } else if (kind_value->text == "loop") {
        kind = Kind::Loop;
    } else if (kind_value->text == "ask_user") {
        kind = Kind::AskUser;
    } else if (kind_value->text == "subagent") {
        kind = Kind::Subagent;
    } else {
        return std::nullopt;
    }

    std::vector<AskUserItem> items;
    if (const json::Value* ask_user = last_member(root, "askUser")) {
        if (ask_user->kind != json::Kind::Array) return std::nullopt;
        for (const json::Value& element : ask_user->elements) {
            if (element.kind != json::Kind::Object) return std::nullopt;

            const json::Value* question = last_member(element, "question");
            if (question == nullptr || question->kind != json::Kind::String) return std::nullopt;

            AskUserItem item;
            item.question = question->text;
            if (const json::Value* answer = last_member(element, "answer")) {
                if (!is_json_null(*answer)) {
                    if (answer->kind != json::Kind::String) return std::nullopt;
                    item.answer = answer->text;
                }
            }
            items.push_back(std::move(item));
        }
    }

    std::vector<std::pair<std::string, std::string>> results;
    if (const json::Value* receipts = last_member(root, "subagentResults")) {
        if (receipts->kind != json::Kind::Object) return std::nullopt;
        for (const json::Member& member : receipts->members) {
            if (member.value.kind != json::Kind::String) return std::nullopt;
            results.emplace_back(member.key, member.value.text);
        }
    }

    std::optional<SubagentResult> subagent_result;
    if (const json::Value* snapshot = last_member(root, "subagentResult")) {
        if (!is_json_null(*snapshot)) {
            if (snapshot->kind != json::Kind::Object) return std::nullopt;
            const json::Value* name = last_member(*snapshot, "name");
            const json::Value* status = last_member(*snapshot, "status");
            if (name == nullptr || name->kind != json::Kind::String || status == nullptr ||
                status->kind != json::Kind::String) {
                return std::nullopt;
            }
            SubagentResult parsed;
            parsed.name = name->text;
            parsed.status = status->text;
            const json::Value* error_text = last_member(*snapshot, "errorText");
            if (error_text != nullptr && !is_json_null(*error_text)) {
                if (error_text->kind != json::Kind::String) return std::nullopt;
                parsed.error_text = error_text->text;
            }
            const json::Value* error_code = last_member(*snapshot, "errorCode");
            if (error_code != nullptr && !is_json_null(*error_code)) {
                if (error_code->kind != json::Kind::String) return std::nullopt;
                parsed.error_code = error_code->text;
            }
            subagent_result = std::move(parsed);
        }
    }

    // MessageSource.init: only ASK_USER may carry items, only SUBAGENT carries receipts, and the
    // display snapshot exists only alongside exactly one receipt.
    if (kind != Kind::AskUser && !items.empty()) return std::nullopt;
    if (kind != Kind::Subagent && !results.empty()) return std::nullopt;
    if (subagent_result.has_value() && !(kind == Kind::Subagent && results.size() == 1)) {
        return std::nullopt;
    }

    Source source;
    source.kind = kind;
    source.ask_user = std::move(items);
    source.subagent_results = std::move(results);
    source.subagent_result = std::move(subagent_result);
    return source;
}

std::optional<std::string> encode(const std::optional<Source>& source) {
    if (!source.has_value()) return std::nullopt;

    std::string out = "{\"kind\":\"";
    out += kind_name(source->kind);
    out += '"';
    if (source->kind == Kind::AskUser && !source->ask_user.empty()) {
        out += ",\"askUser\":[";
        for (std::size_t index = 0; index < source->ask_user.size(); ++index) {
            if (index != 0) out += ',';
            const AskUserItem& item = source->ask_user[index];
            out += "{\"question\":";
            append_string(out, item.question);
            if (item.answer.has_value()) {
                out += ",\"answer\":";
                append_string(out, *item.answer);
            }
            out += '}';
        }
        out += ']';
    }
    if (!source->subagent_results.empty()) {
        out += ",\"subagentResults\":{";
        for (std::size_t index = 0; index < source->subagent_results.size(); ++index) {
            if (index != 0) out += ',';
            append_string(out, source->subagent_results[index].first);
            out += ':';
            append_string(out, source->subagent_results[index].second);
        }
        out += '}';
    }
    if (source->subagent_result.has_value()) {
        out += ",\"subagentResult\":{\"name\":";
        append_string(out, source->subagent_result->name);
        out += ",\"status\":";
        append_string(out, source->subagent_result->status);
        if (source->subagent_result->error_text.has_value()) {
            out += ",\"errorText\":";
            append_string(out, *source->subagent_result->error_text);
        }
        if (source->subagent_result->error_code.has_value()) {
            out += ",\"errorCode\":";
            append_string(out, *source->subagent_result->error_code);
        }
        out += '}';
    }
    out += '}';
    return out;
}

std::optional<std::string> sanitize_imported(const std::optional<std::string>& raw,
                                             Participant participant) {
    if (participant != Participant::User) return std::nullopt;
    return encode(decode(raw));
}

std::optional<Source> for_automation_request_kind(const std::optional<std::string>& request_kind) {
    if (!request_kind.has_value()) return std::nullopt;
    if (*request_kind == "task") {
        Source source;
        source.kind = Kind::Task;
        return source;
    }
    if (*request_kind == "loop") {
        Source source;
        source.kind = Kind::Loop;
        return source;
    }
    if (*request_kind == "subagent") {
        Source source;
        source.kind = Kind::Subagent;
        return source;
    }
    return std::nullopt;
}

std::string ask_user_readable_text(const std::vector<AskUserItem>& items) {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) out += "\n\n";
        out += items[index].question;
        out += '\n';
        out += items[index].answer.value_or(kNoAnswer);
    }
    return out;
}

std::string describe(const std::optional<Source>& source) {
    if (!source.has_value()) return "null";
    std::string out = "MessageSource(kind=";
    switch (source->kind) {
        case Kind::Task:
            out += "TASK";
            break;
        case Kind::Loop:
            out += "LOOP";
            break;
        case Kind::AskUser:
            out += "ASK_USER";
            break;
        case Kind::Subagent:
            out += "SUBAGENT";
            break;
    }
    out += ", askUser=[";
    for (std::size_t index = 0; index < source->ask_user.size(); ++index) {
        if (index != 0) out += ", ";
        const AskUserItem& item = source->ask_user[index];
        out += "AskUserItem(question=";
        out += item.question;
        out += ", answer=";
        out += item.answer.has_value() ? *item.answer : std::string("null");
        out += ')';
    }
    out += "], subagentResults={";
    for (std::size_t index = 0; index < source->subagent_results.size(); ++index) {
        if (index != 0) out += ", ";
        out += source->subagent_results[index].first;
        out += '=';
        out += source->subagent_results[index].second;
    }
    out += "}, subagentResult=";
    if (source->subagent_result.has_value()) {
        const SubagentResult& snapshot = *source->subagent_result;
        out += "SubagentResult(name=";
        out += snapshot.name;
        out += ", status=";
        out += snapshot.status;
        out += ", errorText=";
        out += snapshot.error_text.has_value() ? *snapshot.error_text : std::string("null");
        out += ", errorCode=";
        out += snapshot.error_code.has_value() ? *snapshot.error_code : std::string("null");
        out += ')';
    } else {
        out += "null";
    }
    out += ')';
    return out;
}

}  // namespace agora::core::message_source
