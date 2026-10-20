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

    // MessageSource.init: only ASK_USER may carry items.
    if (kind != Kind::AskUser && !items.empty()) return std::nullopt;

    Source source;
    source.kind = kind;
    source.ask_user = std::move(items);
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
    out += "])";
    return out;
}

}  // namespace agora::core::message_source
