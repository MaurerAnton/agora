#include "agora/server/submission_messages.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

#include "agora/core/json.hpp"

namespace agora::server {
namespace json = ::agora::core::json;  // sibling namespace of agora::server

namespace {

constexpr std::size_t kMaxConfigBytes = 1024 * 1024;

/** Python len(str): number of code points (an astral pair counts once, unlike utf16_length). */
std::size_t code_point_length(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

/** Last member wins for duplicate keys, like a Python dict built by json.loads. */
const json::Value* last_member(const json::Value& object, std::string_view key) {
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) {
            found = &member.value;
        }
    }
    return found;
}

bool required_text(const json::Value& message, std::string_view key, std::size_t limit,
                   std::string& out) {
    const json::Value* value = last_member(message, key);
    if (value == nullptr || value->kind != json::Kind::String) {
        return false;
    }
    std::string trimmed = json::trim(value->text);
    if (trimmed.empty() || code_point_length(trimmed) > limit) {
        return false;
    }
    out = std::move(trimmed);
    return true;
}

}  // namespace

std::optional<SubmissionMessage> message_for(std::string_view package_name) {
    const std::string path = env_value("AGORA_SUBMISSION_MESSAGES");
    if (path.empty() || package_name.empty()) {
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::string raw(kMaxConfigBytes + 1, '\0');
    stream.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    raw.resize(static_cast<std::size_t>(stream.gcount()));
    if (raw.size() > kMaxConfigBytes) {
        return std::nullopt;  // python: read(MAX+1) then len(raw) > MAX
    }

    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        return std::nullopt;
    }

    const json::Value* message = last_member(*parsed.value, package_name);
    if (message == nullptr || message->kind != json::Kind::Object) {
        return std::nullopt;
    }

    SubmissionMessage result;
    if (!required_text(*message, "id", 128, result.id)) {
        return std::nullopt;
    }
    if (!required_text(*message, "title", 200, result.title)) {
        return std::nullopt;
    }
    if (!required_text(*message, "body", 8000, result.body)) {
        return std::nullopt;
    }

    if (const json::Value* button = last_member(*message, "buttonText");
        button != nullptr && button->kind == json::Kind::String) {
        std::string trimmed = json::trim(button->text);
        const std::size_t length = code_point_length(trimmed);
        if (length > 0 && length <= 80) {
            result.button_text = std::move(trimmed);
        }
    }
    return result;
}

}  // namespace agora::server
