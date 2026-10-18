#include "agora/core/provider_support.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::provider_support {
namespace {

namespace json = agora::core::json;

// ---------------------------------------------------------------------------------------------
// JsonPrimitive helpers — kotlinx semantics pinned here: content is trimmed, and the JsonNull
// singleton never carries a message (a QUOTED "null" string does).
// ---------------------------------------------------------------------------------------------

const json::Value* member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;  // last duplicate wins (LinkedHashMap)
    }
    return found;
}

std::optional<std::string> non_blank_primitive(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::Literal && value->text == "null") {
        return std::nullopt;  // JsonNull identity check: null never carries a message
    }
    if (value->kind != json::Kind::String && value->kind != json::Kind::Literal) {
        return std::nullopt;
    }
    const std::string content = json::trim(value->text);
    if (content.empty()) return std::nullopt;
    return content;
}

std::optional<std::string> primitive(const json::Value& object, std::string_view key) {
    return non_blank_primitive(member(object, key));
}

/** firstErrorMessage: message | detail | reason | error_description. */
std::optional<std::string> first_error_message(const json::Value& object) {
    if (auto value = primitive(object, "message")) return value;
    if (auto value = primitive(object, "detail")) return value;
    if (auto value = primitive(object, "reason")) return value;
    return primitive(object, "error_description");
}

std::optional<ParsedProviderHttpErrorBody> parse_provider_http_error_object(
    const json::Value& root) {
    const json::Value* error_value = member(root, "error");
    const json::Value* nested =
        (error_value != nullptr && error_value->kind == json::Kind::Object) ? error_value
                                                                           : nullptr;

    std::optional<std::string> message;
    if (nested != nullptr) message = first_error_message(*nested);
    if (!message) message = non_blank_primitive(error_value);
    if (!message) message = first_error_message(root);
    if (!message) return std::nullopt;

    std::optional<std::string> code;
    if (nested != nullptr) code = primitive(*nested, "code");
    if (!code) code = primitive(root, "code");

    std::optional<std::string> type;
    if (nested != nullptr) {
        type = primitive(*nested, "type");
        if (!type) type = primitive(*nested, "status");
    }
    if (!type) type = primitive(root, "type");
    if (!type) type = primitive(root, "status");

    return ParsedProviderHttpErrorBody{code, type, *message, true};
}

// ---------------------------------------------------------------------------------------------
// BaseUrlResolver mirrors
// ---------------------------------------------------------------------------------------------

std::string trim_trailing_slashes(std::string_view value) {
    std::size_t end = value.size();
    while (end > 0 && value[end - 1] == '/') --end;
    return std::string(value.substr(0, end));
}

/** True when [at] holds a `/v<digit>` pair (the VERSION_SEGMENT pattern, case-sensitive). */
bool version_marker_at(std::string_view text, std::size_t at) {
    return at + 2 < text.size() && text[at] == '/' && text[at + 1] == 'v' &&
           text[at + 2] >= '0' && text[at + 2] <= '9';
}

/** The TRAILING_VERSION_SEGMENT runs with RegexOption.IGNORE_CASE, so `V` matches too. */
bool version_marker_at_ignore_case(std::string_view text, std::size_t at) {
    return at + 2 < text.size() && text[at] == '/' &&
           (text[at + 1] == 'v' || text[at + 1] == 'V') &&
           text[at + 2] >= '0' && text[at + 2] <= '9';
}

bool is_class_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

/** Java's default `$`: end of input, or before THE final line terminator. */
bool ends_at_anchor(std::string_view text, std::size_t at) {
    if (at == text.size()) return true;
    const std::string_view rest = text.substr(at);
    if (rest == "\n" || rest == "\r") return true;
    if (rest == "\r\n") return true;
    if (rest == "\xC2\x85") return true;   // NEL  (U+0085)
    if (rest == "\xE2\x80\xA8") return true;   // LS   (U+2028)
    if (rest == "\xE2\x80\xA9") return true;   // PS   (U+2029)
    return false;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ProviderHttpError.kt
// ---------------------------------------------------------------------------------------------

std::optional<ParsedProviderHttpErrorBody> parse_provider_http_error_body(
    std::string_view raw_body) {
    const std::string trimmed = json::trim(raw_body);
    if (trimmed.empty()) return std::nullopt;

    ParsedProviderHttpErrorBody unstructured{std::nullopt, std::nullopt, trimmed, false};
    const json::ParseResult parsed = json::parse(trimmed);
    if (!parsed.value) return unstructured;  // malformed JSON: the raw text is the message

    const json::Value& root = *parsed.value;
    if (root.kind == json::Kind::Object) {
        const auto structured = parse_provider_http_error_object(root);
        if (structured) return structured;
        return unstructured;
    }
    if (root.kind == json::Kind::String || root.kind == json::Kind::Literal) {
        if (const auto message = non_blank_primitive(&root)) {
            return ParsedProviderHttpErrorBody{std::nullopt, std::nullopt, *message, true};
        }
        return unstructured;
    }
    return unstructured;
}

sn::ErrorApi provider_http_error(std::int32_t status_code,
                                 const std::optional<std::string>& raw_body) {
    std::optional<ParsedProviderHttpErrorBody> parsed;
    if (raw_body) parsed = parse_provider_http_error_body(*raw_body);
    if (!parsed) {
        return sn::ErrorApi{std::nullopt, std::nullopt,
                            "HTTP " + std::to_string(status_code)};
    }
    sn::ErrorApi error;
    error.code = parsed->code ? *parsed->code : std::to_string(status_code);
    error.type = parsed->type;
    error.message = parsed->message;
    return error;
}

std::optional<std::string> extract_structured_provider_http_error_message(
    std::string_view raw_body) {
    const auto parsed = parse_provider_http_error_body(raw_body);
    if (!parsed || !parsed->structured) return std::nullopt;
    return parsed->message;
}

// ---------------------------------------------------------------------------------------------
// BaseUrlResolver.kt
// ---------------------------------------------------------------------------------------------

bool has_version_segment(std::string_view url) {
    const std::string trimmed = trim_trailing_slashes(url);
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
        if (version_marker_at(trimmed, i)) return true;
    }
    return false;
}

std::string with_v1(std::string_view url) {
    const std::string trimmed = trim_trailing_slashes(url);
    if (trimmed.empty() || json::is_blank(trimmed) || has_version_segment(trimmed)) {
        return trimmed;
    }
    return trimmed + "/v1";
}

std::optional<std::string> without_trailing_version(std::string_view url) {
    const std::string trimmed = trim_trailing_slashes(url);
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
        if (!version_marker_at_ignore_case(trimmed, i)) continue;
        std::size_t end = i + 3;  // after `/v<digit>`
        while (end < trimmed.size() && is_class_char(trimmed[end])) ++end;
        if (!ends_at_anchor(trimmed, end)) continue;
        // removeRange(match.range): the anchor's terminator stays, then trimEnd('/').
        std::string removed = trimmed.substr(0, i) + trimmed.substr(end);
        removed = trim_trailing_slashes(removed);
        if (json::is_blank(removed)) return std::nullopt;
        return removed;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// ProviderDefaults.kt
// ---------------------------------------------------------------------------------------------

bool is_openai_compatible_embedding(std::string_view name) {
    return name == "OpenAI" || name == "Open Router";  // Constants.PROVIDER_OPENAI / _OPEN_ROUTER
}

std::string open_ai_compatible_base_url(const std::map<std::string, std::string>& base_urls) {
    const auto found = base_urls.find("OpenAI");
    if (found != base_urls.end()) return found->second;
    return std::string(kOpenAiBaseUrl);
}

std::string embedding_base_url(std::string_view provider) {
    const std::string key = lower_ascii(std::string(provider));  // String.lowercase() — ASCII fold
    if (key == "openai") return std::string(kOpenAiBaseUrl);
    if (key == "mistral") return "https://api.mistral.ai/v1";
    if (key == "open router" || key == "openrouter") return "https://openrouter.ai/api/v1";
    if (key == "voyage ai" || key == "voyage") return "https://api.voyageai.com/v1";
    if (key == "siliconflow") return "https://api.siliconflow.cn/v1";
    if (key == "ollama") return "http://localhost:11434/v1";
    return std::string(kOpenAiBaseUrl);
}

// ---------------------------------------------------------------------------------------------
// CustomProviderNamePolicy.kt
// ---------------------------------------------------------------------------------------------

const std::vector<std::string>& reserved_provider_names() {
    static const std::vector<std::string> names = {
        "Google",       // Constants.PROVIDER_GOOGLE
        "OpenAI",       // Constants.PROVIDER_OPENAI
        "Anthropic",    // Constants.PROVIDER_ANTHROPIC
        "DeepSeek",     // Constants.PROVIDER_DEEPSEEK
        "Qwen",         // Constants.PROVIDER_QWEN
        "Groq",         // Constants.PROVIDER_GROQ
        "Ollama",       // Constants.PROVIDER_OLLAMA
        "Open Router",  // Constants.PROVIDER_OPEN_ROUTER
        "OpenCode Go",  // Constants.PROVIDER_OPENCODE_GO
        "Local",        // Constants.PROVIDER_LOCAL
    };
    return names;
}

bool is_allowed_custom_provider_name(std::string_view name) {
    const std::string candidate = json::trim(name);
    if (candidate.empty()) return false;
    for (const std::string& reserved : reserved_provider_names()) {
        if (lower_ascii(reserved) == lower_ascii(candidate)) return false;  // ignoreCase
    }
    return true;
}

bool has_custom_provider_name_conflict(std::string_view name,
                                       const std::vector<std::string>& existing_names,
                                       const std::optional<std::string>& current_name) {
    const std::string candidate = json::trim(name);
    if (!is_allowed_custom_provider_name(candidate)) return true;

    const std::string folded = lower_ascii(candidate);
    for (const std::string& existing : existing_names) {
        const bool is_current_entry = current_name.has_value() && existing == *current_name;
        if (is_current_entry) continue;
        if (lower_ascii(json::trim(existing)) == folded) return true;
    }
    return false;
}

CustomProviderSanitizationResult sanitize_custom_provider_names(
    const std::vector<std::string>& names) {
    CustomProviderSanitizationResult result;
    std::vector<std::string> accepted_names;
    for (const std::string& name : names) {
        // The original (untrimmed) name is what gets carried into the result.
        if (has_custom_provider_name_conflict(name, accepted_names)) {
            result.rejected.push_back(name);
        } else {
            result.accepted.push_back(name);
            accepted_names.push_back(name);  // the raw name, exactly like the Kotlin list
        }
    }
    return result;
}

}  // namespace agora::core::provider_support
