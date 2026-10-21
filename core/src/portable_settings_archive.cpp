// Port of data/PortableSettingsArchive.kt (both halves).
//
// The export half builds the settings.json payload in source order with kotlinx's
// encodeDefaults=false rule: a model field equal to its declared default is omitted. The restore
// half is a straight translation of restoreFromJsonObject: nullable accessors first, typed decode
// behind runCatching, merges through LinkedHashMap/LinkedHashSet order, then the save calls in
// source order. Entropy and the prompt resolver are parameters (see the header).
#include "agora/core/portable_settings_archive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_set>

#include "agora/core/native_backup_format.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/openai_service_tiers.hpp"
#include "agora/core/provider_identity.hpp"
#include "agora/core/thinking_capability.hpp"

namespace agora::core::settings_archive {
namespace {

namespace json = agora::core::json;
using json::Kind;
using json::Value;

using provider_identity::Provider;

// -------------------------------------------------------------------------------------- helpers

const Value* field(const Value& object, std::string_view name) {
    const Value* found = nullptr;
    if (object.kind != Kind::Object) return nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) found = &member.value;  // last duplicate wins
    }
    return found;
}

bool has_field(const Value& object, std::string_view name) {
    return field(object, name) != nullptr;
}

bool is_null_literal(const Value& value) {
    return value.kind == Kind::Literal && value.text == "null";
}

bool is_primitive(const Value& value) {
    return value.kind == Kind::String || value.kind == Kind::Literal;
}

/** JsonObject.string(): (get(key) as? JsonPrimitive)?.contentOrNull. */
std::optional<std::string> json_string(const Value* value) {
    if (value == nullptr || !is_primitive(*value)) return std::nullopt;
    if (is_null_literal(*value)) return std::nullopt;
    return value->text;
}

std::optional<std::string> string_field(const Value& object, std::string_view name) {
    return json_string(field(object, name));
}

/** JsonObject.nullableString(): JsonNull -> null, otherwise string(). */
std::optional<std::string> nullable_string_field(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value != nullptr && is_null_literal(*value)) return std::nullopt;
    return json_string(value);
}

std::optional<bool> json_boolean(const Value* value) {
    if (value == nullptr || !is_primitive(*value)) return std::nullopt;
    std::string lowered = value->text;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lowered == "true") return true;
    if (lowered == "false") return false;
    return std::nullopt;
}

std::optional<bool> boolean_field(const Value& object, std::string_view name) {
    return json_boolean(field(object, name));
}

/**
 * java.lang.String.trim() for the reference numeric parser: only bytes <= U+0020 are stripped,
 * so NBSP and em space stay in the content and fail the parse (pinned by the accessor probes).
 */
std::string trim_ascii(std::string_view raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && static_cast<unsigned char>(raw[begin]) <= 0x20) ++begin;
    while (end > begin && static_cast<unsigned char>(raw[end - 1]) <= 0x20) --end;
    return std::string(raw.substr(begin, end - begin));
}

/**
 * The decimal literal grammar behind JsonPrimitive.int: optional '-', ASCII digits (leading zeros
 * allowed) and an optional decimal exponent; a negative exponent must not leave a fractional part;
 * the result must fit in Int. '+' and '.' are rejected. Returns nullopt for intOrNull-style
 * accessors; the throwing wrapper of the legacy reader maps the same failure to NumberFormatException.
 */
std::optional<int> parse_int_literal(std::string_view raw) {
    const std::string text = trim_ascii(raw);
    if (text.empty()) return std::nullopt;

    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '-') {
        negative = true;
        ++pos;
    }
    const std::size_t digits_begin = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos == digits_begin) return std::nullopt;

    long long exponent = 0;
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        bool exponent_negative = false;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            exponent_negative = text[pos] == '-';
            ++pos;
        }
        const std::size_t exponent_begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (exponent < 100000) exponent = exponent * 10 + (text[pos] - '0');
            ++pos;
        }
        if (pos != text.size()) return std::nullopt;
        if (pos == exponent_begin) exponent = 0;
        if (exponent_negative) exponent = -exponent;
    }
    if (pos != text.size()) return std::nullopt;

    const std::size_t mantissa_end = text.find_first_of("eE", digits_begin);
    std::string digits =
        text.substr(digits_begin, mantissa_end == std::string::npos
                                      ? std::string::npos
                                      : mantissa_end - digits_begin);
    const std::size_t first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return 0;
    digits.erase(0, first_nonzero);

    if (exponent < 0) {
        const auto drop = static_cast<std::size_t>(-exponent);
        if (digits.size() <= drop) return std::nullopt;
        for (std::size_t i = digits.size() - drop; i < digits.size(); ++i) {
            if (digits[i] != '0') return std::nullopt;
        }
        digits.resize(digits.size() - drop);
    } else {
        if (exponent > 20) return std::nullopt;
        digits.append(static_cast<std::size_t>(exponent), '0');
    }

    if (digits.size() > 10) return std::nullopt;
    long long magnitude = 0;
    for (char digit : digits) magnitude = magnitude * 10 + (digit - '0');
    if (negative) magnitude = -magnitude;
    if (magnitude < std::numeric_limits<int>::min() ||
        magnitude > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(magnitude);
}

std::optional<int> int_field(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || !is_primitive(*value)) return std::nullopt;
    if (is_null_literal(*value)) return std::nullopt;
    return parse_int_literal(value->text);
}

/**
 * java.lang.Float.parseFloat for the primitive content: the exact "NaN"/"Infinity" spellings only
 * (lowercase inf/nan are rejected, unlike strtof), decimal and hex forms with a mandatory p
 * exponent, a trailing f/F/d/D suffix and leading/trailing whitespace. Overflow to infinity and
 * underflow to zero are Java behaviour.
 */
std::optional<float> parse_float_literal(std::string_view raw) {
    std::string text = trim_ascii(raw);
    if (text.empty()) return std::nullopt;

    const std::size_t sign = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    const std::string_view body(text.data() + sign, text.size() - sign);
    if (body == "NaN" || body == "Infinity") {
        if (body == "NaN") return std::numeric_limits<float>::quiet_NaN();
        return text[0] == '-' ? -std::numeric_limits<float>::infinity()
                              : std::numeric_limits<float>::infinity();
    }

    if (text.size() > 1) {
        const char last = text.back();
        if (last == 'f' || last == 'F' || last == 'd' || last == 'D') text.pop_back();
    }
    if (text.empty()) return std::nullopt;

    const std::size_t start = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    const bool hex = text.compare(start, 2, "0x") == 0 || text.compare(start, 2, "0X") == 0;
    if (hex && text.find_first_of("pP", start + 2) == std::string::npos) return std::nullopt;
    if (start < text.size() && std::isalpha(static_cast<unsigned char>(text[start])) != 0) {
        if (!hex) return std::nullopt;
    }

    char* parse_end = nullptr;
    const float value = std::strtof(text.c_str(), &parse_end);
    if (parse_end != text.c_str() + text.size()) return std::nullopt;
    return value;
}

std::optional<float> float_field(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || !is_primitive(*value)) return std::nullopt;
    if (is_null_literal(*value)) return std::nullopt;
    return parse_float_literal(value->text);
}

// ------------------------------------------------------------------------------- typed decoders

/** LinkedHashMap put: an existing key keeps its position and receives the new value. */
template <typename K, typename V>
void put_or_assign(std::vector<std::pair<K, V>>& entries, const K& key, V value);

std::optional<std::vector<std::string>> decode_string_set(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Array) return std::nullopt;
    std::vector<std::string> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) return std::nullopt;
        out.push_back(element.text);
    }
    return out;
}

std::optional<StringMap> decode_string_map(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Object) return std::nullopt;
    StringMap out;
    for (const json::Member& member : value->members) {
        if (member.value.kind != Kind::String) return std::nullopt;
        put_or_assign(out, member.key, member.value.text);
    }
    return out;
}

std::optional<BoolMap> decode_bool_map(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Object) return std::nullopt;
    BoolMap out;
    for (const json::Member& member : value->members) {
        const std::optional<bool> flag = json_boolean(&member.value);
        if (!flag.has_value()) return std::nullopt;
        put_or_assign(out, member.key, *flag);
    }
    return out;
}

std::optional<EmbeddingModelConfig> decode_embedding(const Value& value) {
    if (value.kind != Kind::Object) return std::nullopt;
    EmbeddingModelConfig model;
    const Value* id = field(value, "id");
    if (id != nullptr) {
        if (id->kind != Kind::String) return std::nullopt;
        model.id = id->text;
    }  // missing id: Kotlin mints a random UUID — scenarios always supply ids (see README)
    const Value* name = field(value, "name");
    if (name == nullptr || name->kind != Kind::String) return std::nullopt;
    model.name = name->text;
    const Value* type = field(value, "type");
    if (type == nullptr || type->kind != Kind::String) return std::nullopt;
    if (type->text == "REMOTE") {
        model.type = EmbeddingModelType::Remote;
    } else if (type->text == "LOCAL") {
        model.type = EmbeddingModelType::Local;
    } else {
        return std::nullopt;
    }
    auto optional_string = [&](std::string_view key, std::string* out) -> bool {
        const Value* member = field(value, key);
        if (member == nullptr) return true;
        if (member->kind != Kind::String) return false;
        *out = member->text;
        return true;
    };
    auto int_member = [&](std::string_view key, int* out) -> bool {
        const Value* member = field(value, key);
        if (member == nullptr) return true;
        if (member->kind != Kind::Literal) return false;
        const std::optional<int> parsed = parse_int_literal(member->text);
        if (!parsed.has_value()) return false;
        *out = *parsed;
        return true;
    };
    if (!optional_string("remoteModelName", &model.remote_model_name)) return std::nullopt;
    if (!optional_string("remoteBaseUrl", &model.remote_base_url)) return std::nullopt;
    if (!optional_string("remoteApiKey", &model.remote_api_key)) return std::nullopt;
    if (!optional_string("localFilePath", &model.local_file_path)) return std::nullopt;
    if (!int_member("batchSize", &model.batch_size)) return std::nullopt;
    return model;
}

std::optional<std::vector<EmbeddingModelConfig>> decode_embedding_list(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Array) return std::nullopt;
    std::vector<EmbeddingModelConfig> out;
    for (const Value& element : value->elements) {
        std::optional<EmbeddingModelConfig> model = decode_embedding(element);
        if (!model.has_value()) return std::nullopt;
        out.push_back(std::move(*model));
    }
    return out;
}

std::optional<ShellDeviceConfig> decode_shell_device(const Value& value) {
    if (value.kind != Kind::Object) return std::nullopt;
    ShellDeviceConfig device;
    const Value* id = field(value, "id");
    if (id != nullptr) {
        if (id->kind != Kind::String) return std::nullopt;
        device.id = id->text;
    }
    const Value* name = field(value, "name");
    if (name == nullptr || name->kind != Kind::String) return std::nullopt;
    device.name = name->text;
    auto optional_string = [&](std::string_view key, std::string* out) -> bool {
        const Value* member = field(value, key);
        if (member == nullptr) return true;
        if (member->kind != Kind::String) return false;
        *out = member->text;
        return true;
    };
    if (!optional_string("description", &device.description)) return std::nullopt;
    if (!optional_string("type", &device.type)) return std::nullopt;
    if (!optional_string("serverUrl", &device.server_url)) return std::nullopt;
    if (!optional_string("apiKey", &device.api_key)) return std::nullopt;
    if (!optional_string("conchPublicKey", &device.conch_public_key)) return std::nullopt;
    if (!optional_string("sshHost", &device.ssh_host)) return std::nullopt;
    const Value* ssh_port = field(value, "sshPort");
    if (ssh_port != nullptr) {
        if (ssh_port->kind != Kind::Literal) return std::nullopt;
        const std::optional<int> parsed = parse_int_literal(ssh_port->text);
        if (!parsed.has_value()) return std::nullopt;
        device.ssh_port = *parsed;
    }
    if (!optional_string("sshUser", &device.ssh_user)) return std::nullopt;
    if (!optional_string("sshPassword", &device.ssh_password)) return std::nullopt;
    if (!optional_string("sshHostKey", &device.ssh_host_key)) return std::nullopt;
    return device;
}

std::optional<std::vector<ShellDeviceConfig>> decode_shell_list(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Array) return std::nullopt;
    std::vector<ShellDeviceConfig> out;
    for (const Value& element : value->elements) {
        std::optional<ShellDeviceConfig> device = decode_shell_device(element);
        if (!device.has_value()) return std::nullopt;
        out.push_back(std::move(*device));
    }
    return out;
}

std::optional<McpServerConfig> decode_mcp_server(const Value& value) {
    if (value.kind != Kind::Object) return std::nullopt;
    McpServerConfig server;
    const Value* id = field(value, "id");
    if (id != nullptr) {
        if (id->kind != Kind::String) return std::nullopt;
        server.id = id->text;
    }
    const Value* name = field(value, "name");
    if (name != nullptr) {
        if (name->kind != Kind::String) return std::nullopt;
        server.name = name->text;
    }
    const Value* enabled = field(value, "enabled");
    if (enabled != nullptr) {
        const std::optional<bool> parsed = json_boolean(enabled);
        if (!parsed.has_value()) return std::nullopt;
        server.enabled = *parsed;
    }
    const Value* url = field(value, "url");
    if (url != nullptr) {
        if (url->kind != Kind::String) return std::nullopt;
        server.url = url->text;
    }
    const Value* transport = field(value, "transport");
    if (transport != nullptr) {
        if (transport->kind != Kind::String) return std::nullopt;
        if (transport->text != "streamable_http" && transport->text != "sse") return std::nullopt;
        server.transport = transport->text;
    }
    const Value* headers = field(value, "headers");
    if (headers != nullptr) {
        if (headers->kind != Kind::Object) return std::nullopt;
        StringMap parsed_headers;
        for (const json::Member& member : headers->members) {
            if (member.value.kind != Kind::String) return std::nullopt;
            put_or_assign(parsed_headers, member.key, member.value.text);
        }
        server.headers = std::move(parsed_headers);
    }
    const Value* disabled_tools = field(value, "disabledTools");
    if (disabled_tools != nullptr) {
        if (disabled_tools->kind != Kind::Array) return std::nullopt;
        std::vector<std::string> tools;
        for (const Value& element : disabled_tools->elements) {
            if (element.kind != Kind::String) return std::nullopt;
            tools.push_back(element.text);
        }
        server.disabled_tools = std::move(tools);
    }
    return server;
}

std::optional<std::vector<McpServerConfig>> decode_mcp_list(const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Array) return std::nullopt;
    std::vector<McpServerConfig> out;
    for (const Value& element : value->elements) {
        std::optional<McpServerConfig> server = decode_mcp_server(element);
        if (!server.has_value()) return std::nullopt;
        out.push_back(std::move(*server));
    }
    return out;
}

/** CustomEndpointProtocol.fromWireValue: trim + lowercase, aliases, else UNKNOWN. */
std::string decode_protocol(std::string_view wire) {
    std::string lowered = json::trim(wire);
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lowered == "openai") return "openai";
    if (lowered == "google" || lowered == "gemini") return "google";
    if (lowered == "anthropic" || lowered == "claude") return "anthropic";
    return "unknown";
}

std::optional<CustomProviderConfig> decode_custom_provider(const Value& value) {
    if (value.kind != Kind::Object) return std::nullopt;
    CustomProviderConfig provider;
    const Value* name = field(value, "name");
    if (name == nullptr || name->kind != Kind::String) return std::nullopt;
    provider.name = name->text;
    const Value* protocol = field(value, "protocol");
    if (protocol != nullptr) {
        if (protocol->kind != Kind::String) return std::nullopt;
        provider.protocol = decode_protocol(protocol->text);
    }
    const Value* id = field(value, "id");
    if (id != nullptr) {
        if (id->kind != Kind::String) return std::nullopt;
        provider.id = id->text;
    }
    const Value* legacy_names = field(value, "legacyNames");
    if (legacy_names != nullptr) {
        if (legacy_names->kind != Kind::Array) return std::nullopt;
        for (const Value& element : legacy_names->elements) {
            if (element.kind != Kind::String) return std::nullopt;
            provider.legacy_names.push_back(element.text);
        }
    }
    auto bool_member = [&](std::string_view key, bool* out) -> bool {
        const Value* member = field(value, key);
        if (member == nullptr) return true;
        const std::optional<bool> parsed = json_boolean(member);
        if (!parsed.has_value()) return false;
        *out = *parsed;
        return true;
    };
    if (!bool_member("responsesApiEnabled", &provider.responses_api_enabled)) {
        return std::nullopt;
    }
    if (!bool_member("anthropicCacheEnabled", &provider.anthropic_cache_enabled)) {
        return std::nullopt;
    }
    const Value* ttl = field(value, "anthropicCacheTtl");
    if (ttl != nullptr) {
        if (ttl->kind != Kind::String) return std::nullopt;
        provider.anthropic_cache_ttl = ttl->text;
    }
    return provider;
}

std::optional<std::vector<CustomProviderConfig>> decode_custom_provider_list(
    const Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Array) return std::nullopt;
    std::vector<CustomProviderConfig> out;
    for (const Value& element : value->elements) {
        std::optional<CustomProviderConfig> provider = decode_custom_provider(element);
        if (!provider.has_value()) return std::nullopt;
        out.push_back(std::move(*provider));
    }
    return out;
}

// --------------------------------------------------------------------------- ordered containers

/** LinkedHashMap put: an existing key keeps its position and receives the new value. */
template <typename K, typename V>
void put_or_assign(std::vector<std::pair<K, V>>& entries, const K& key, V value) {
    for (auto& entry : entries) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    entries.emplace_back(key, std::move(value));
}

/** Kotlin `left + right`: left order, right values overwrite in place, new keys append. */
template <typename K, typename V>
std::vector<std::pair<K, V>> merge_maps(const std::vector<std::pair<K, V>>& left,
                                        const std::vector<std::pair<K, V>>& right) {
    std::vector<std::pair<K, V>> merged = left;
    for (const auto& entry : right) put_or_assign(merged, entry.first, entry.second);
    return merged;
}

/** LinkedHashSet `left + imported`: left order, imported appends unseen keys. */
std::vector<std::string> merge_sets(const std::vector<std::string>& left,
                                    const std::vector<std::string>& right) {
    std::vector<std::string> merged = left;
    for (const std::string& value : right) {
        if (std::find(merged.begin(), merged.end(), value) == merged.end()) {
            merged.push_back(value);
        }
    }
    return merged;
}

/** Kotlin `associateTo(LinkedHashMap)`: duplicate keys keep the first position, last value wins. */
template <typename V>
std::vector<std::pair<std::string, V>> associate_to(const std::vector<std::string>& keys,
                                                    const std::vector<V>& values) {
    std::vector<std::pair<std::string, V>> out;
    for (std::size_t i = 0; i < keys.size(); ++i) put_or_assign(out, keys[i], values[i]);
    return out;
}

/** PortableSettingsArchive.mergeById: LinkedHashMap, existing then imported. */
template <typename T, typename K>
std::vector<T> merge_by_id(const std::vector<T>& existing, const std::vector<T>& imported,
                           const std::function<K(const T&)>& key) {
    std::vector<T> merged;
    std::vector<K> keys;
    for (const T& value : existing) {
        const K value_key = key(value);
        bool replaced = false;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == value_key) {
                merged[i] = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            keys.push_back(value_key);
            merged.push_back(value);
        }
    }
    for (const T& value : imported) {
        const K value_key = key(value);
        bool replaced = false;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == value_key) {
                merged[i] = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            keys.push_back(value_key);
            merged.push_back(value);
        }
    }
    return merged;
}

/** SettingsDefaults.kt::importedContextCompactThresholdPercent. */
std::optional<int> imported_context_compact_threshold(std::optional<int> value) {
    if (!value.has_value()) return std::nullopt;
    if (*value < 50 || *value > 100) return std::nullopt;
    return value;
}

bool contains(const std::vector<std::string>& values, std::string_view wanted) {
    for (const std::string& value : values) {
        if (value == wanted) return true;
    }
    return false;
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += ", ";
        out += names[i];
    }
    return out;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// ----------------------------------------------------------------------------- prepare providers

struct ImportedProviderIdentities {
    std::vector<CustomProviderConfig> providers;
    std::vector<CustomProviderConfig> rejected;
    ReferenceMap model_reference_remap;
    ReferenceMap provider_name_remap;
};

Provider to_identity_provider(const CustomProviderConfig& config) {
    Provider provider;
    provider.name = config.name;
    provider.id = config.id;
    provider.legacy_names = config.legacy_names;
    return provider;
}

bool same_identity(const Provider& left, const Provider& right) {
    return left.name == right.name && left.id == right.id &&
           left.legacy_names == right.legacy_names;
}

/**
 * CustomProviderNamePolicy.sanitize over full configs, reusing the already-pinned identity
 * policy for the decision and walking the two output lists (which preserve the original order)
 * to map each decision back onto its config.
 */
void sanitize_configs(const std::vector<CustomProviderConfig>& configs,
                      std::vector<CustomProviderConfig>* accepted,
                      std::vector<CustomProviderConfig>* rejected) {
    std::vector<Provider> raw;
    for (const CustomProviderConfig& config : configs) raw.push_back(to_identity_provider(config));
    const provider_identity::Sanitization result = provider_identity::sanitize(raw);
    std::size_t accepted_index = 0;
    std::size_t rejected_index = 0;
    for (const CustomProviderConfig& config : configs) {
        const Provider current = to_identity_provider(config);
        if (accepted_index < result.accepted.size() &&
            same_identity(result.accepted[accepted_index], current)) {
            accepted->push_back(config);
            ++accepted_index;
            continue;
        }
        if (rejected_index < result.rejected.size() &&
            same_identity(result.rejected[rejected_index], current)) {
            rejected->push_back(config);
            ++rejected_index;
            continue;
        }
        throw std::logic_error("sanitize partition did not line up with the input order");
    }
}

ImportedProviderIdentities prepare_imported_custom_providers(
    const std::vector<CustomProviderConfig>& raw,
    const std::vector<CustomProviderConfig>& existing, bool replace,
    const IdGenerator& id_generator) {
    for (const CustomProviderConfig& config : raw) {
        if (config.anthropic_cache_ttl != "5m" && config.anthropic_cache_ttl != "1h") {
            throw IllegalArgumentException("Invalid Anthropic cache duration");
        }
    }
    std::vector<CustomProviderConfig> accepted;
    std::vector<CustomProviderConfig> rejected;
    sanitize_configs(raw, &accepted, &rejected);

    std::vector<std::string> occupied;
    if (!replace) {
        for (const CustomProviderConfig& config : existing) {
            const std::string id = config.provider_id();
            if (provider_identity::is_stable_id(id) && !contains(occupied, id)) {
                occupied.push_back(id);
            }
        }
    }

    ImportedProviderIdentities identities;
    for (const CustomProviderConfig& imported : accepted) {
        const std::string imported_reference =
            provider_identity::is_stable_id(imported.id) ? imported.id : imported.name;
        const CustomProviderConfig* matching = nullptr;
        if (!replace) {
            for (const CustomProviderConfig& current : existing) {
                const bool same_id = provider_identity::is_stable_id(imported.id) &&
                                     current.id == imported.id;
                const bool same_name =
                    lower_ascii(current.name) == lower_ascii(imported.name);
                if (same_id || same_name) {
                    matching = &current;
                    break;
                }
            }
        }

        CustomProviderConfig target;
        if (matching != nullptr) {
            target.name = matching->name;
            target.id = matching->provider_id();
            target.protocol = imported.protocol;
            target.responses_api_enabled = imported.responses_api_enabled;
            target.anthropic_cache_enabled = imported.anthropic_cache_enabled;
            target.anthropic_cache_ttl = imported.anthropic_cache_ttl;
            std::vector<std::string> legacy;
            for (const std::string& name : matching->legacy_names) {
                if (!contains(legacy, name)) legacy.push_back(name);
            }
            for (const std::string& name : imported.legacy_names) {
                if (!contains(legacy, name)) legacy.push_back(name);
            }
            if (imported_reference != target.id && !contains(legacy, imported_reference)) {
                legacy.push_back(imported_reference);
            }
            target.legacy_names = std::move(legacy);
        } else {
            const std::vector<Provider> raw_providers = {to_identity_provider(imported)};
            const provider_identity::Normalization normalization =
                provider_identity::normalize(raw_providers, occupied,
                                             [&](const Provider&) { return id_generator(); },
                                             id_generator);
            if (normalization.providers.size() != 1) {
                throw std::logic_error("normalize must yield exactly one provider");
            }
            const Provider& normalized = normalization.providers.front();
            target.name = normalized.name;
            target.id = normalized.id;
            target.legacy_names = normalized.legacy_names;
            target.protocol = imported.protocol;
            target.responses_api_enabled = imported.responses_api_enabled;
            target.anthropic_cache_enabled = imported.anthropic_cache_enabled;
            target.anthropic_cache_ttl = imported.anthropic_cache_ttl;
        }
        if (!contains(occupied, target.provider_id())) {
            occupied.push_back(target.provider_id());
        }
        identities.providers.push_back(target);

        put_or_assign(identities.model_reference_remap, imported_reference,
                      target.provider_id());
        for (const std::string& legacy : imported.legacy_names) {
            put_or_assign(identities.model_reference_remap, legacy, target.provider_id());
        }
        put_or_assign(identities.provider_name_remap, imported.name, target.name);
    }
    identities.rejected = rejected;
    return identities;
}

// --------------------------------------------------------------------------------- export codec

Value string_value(std::string_view text) {
    Value value;
    value.kind = Kind::String;
    value.text = text;
    return value;
}

Value literal(std::string text) {
    Value value;
    value.kind = Kind::Literal;
    value.text = std::move(text);
    return value;
}

Value bool_value(bool flag) { return literal(flag ? "true" : "false"); }
Value int_value(int number) { return literal(std::to_string(number)); }
Value float_value(float number) {
    return literal(agora::core::openai::format_kotlin_float(number));
}
Value null_value() { return literal("null"); }

void put_member(Value& object, std::string key, Value value) {
    object.members.emplace_back(std::move(key), std::move(value));
}

Value object_value() {
    Value value;
    value.kind = Kind::Object;
    return value;
}

Value array_value() {
    Value value;
    value.kind = Kind::Array;
    return value;
}

void put_string_array(Value& object, std::string key, const std::vector<std::string>& items) {
    Value array = array_value();
    for (const std::string& item : items) array.elements.push_back(string_value(item));
    put_member(object, std::move(key), std::move(array));
}

void put_bool_map(Value& object, std::string key, const BoolMap& entries) {
    Value map = object_value();
    for (const auto& entry : entries) put_member(map, entry.first, bool_value(entry.second));
    put_member(object, std::move(key), std::move(map));
}

void put_string_map(Value& object, std::string key, const StringMap& entries) {
    Value map = object_value();
    for (const auto& entry : entries) put_member(map, entry.first, string_value(entry.second));
    put_member(object, std::move(key), std::move(map));
}

// kotlinx encodeDefaults=false: a member is written only when it differs from the default.
void put_if_non_default(Value& object, std::string key, std::string_view value,
                        std::string_view default_value) {
    if (value != default_value) put_member(object, std::move(key), string_value(value));
}

void put_if_non_default(Value& object, std::string key, int value, int default_value) {
    if (value != default_value) put_member(object, std::move(key), int_value(value));
}

void put_if_non_default(Value& object, std::string key, bool value, bool default_value) {
    if (value != default_value) put_member(object, std::move(key), bool_value(value));
}

void put_if_non_default(Value& object, std::string key, const StringMap& value,
                        const StringMap& default_value) {
    if (value != default_value) put_string_map(object, std::move(key), value);
}

void put_if_non_default(Value& object, std::string key, const std::vector<std::string>& value,
                        const std::vector<std::string>& default_value) {
    if (value != default_value) put_string_array(object, std::move(key), value);
}

Value encode_embedding_model(const EmbeddingModelConfig& model) {
    Value out = object_value();
    put_member(out, "id", string_value(model.id));  // default is a random UUID: always emitted
    put_member(out, "name", string_value(model.name));
    put_member(out, "type", string_value(model.type == EmbeddingModelType::Remote ? "REMOTE"
                                                                                 : "LOCAL"));
    put_if_non_default(out, "remoteModelName", model.remote_model_name, "");
    put_if_non_default(out, "remoteBaseUrl", model.remote_base_url, "");
    put_if_non_default(out, "remoteApiKey", model.remote_api_key, "");
    put_if_non_default(out, "localFilePath", model.local_file_path, "");
    put_if_non_default(out, "batchSize", model.batch_size, 8);
    return out;
}

Value encode_shell_device(const ShellDeviceConfig& device) {
    Value out = object_value();
    put_member(out, "id", string_value(device.id));
    put_member(out, "name", string_value(device.name));
    put_if_non_default(out, "description", device.description, "");
    put_if_non_default(out, "type", device.type, "conch");
    put_if_non_default(out, "serverUrl", device.server_url, "");
    put_if_non_default(out, "apiKey", device.api_key, "");
    put_if_non_default(out, "conchPublicKey", device.conch_public_key, "");
    put_if_non_default(out, "sshHost", device.ssh_host, "");
    put_if_non_default(out, "sshPort", device.ssh_port, 22);
    put_if_non_default(out, "sshUser", device.ssh_user, "root");
    put_if_non_default(out, "sshPassword", device.ssh_password, "");
    put_if_non_default(out, "sshHostKey", device.ssh_host_key, "");
    return out;
}

Value encode_mcp_server(const McpServerConfig& server) {
    Value out = object_value();
    put_member(out, "id", string_value(server.id));
    put_if_non_default(out, "name", server.name, "");
    put_if_non_default(out, "enabled", server.enabled, true);
    put_if_non_default(out, "url", server.url, "");
    put_if_non_default(out, "transport", server.transport, "streamable_http");
    put_if_non_default(out, "headers", server.headers, StringMap{});
    put_if_non_default(out, "disabledTools", server.disabled_tools, std::vector<std::string>{});
    return out;
}

Value encode_custom_provider(const CustomProviderConfig& provider) {
    Value out = object_value();
    put_member(out, "name", string_value(provider.name));
    put_if_non_default(out, "protocol", provider.protocol, "openai");
    put_if_non_default(out, "id", provider.id, "");
    put_if_non_default(out, "legacyNames", provider.legacy_names, std::vector<std::string>{});
    put_if_non_default(out, "responsesApiEnabled", provider.responses_api_enabled, false);
    put_if_non_default(out, "anthropicCacheEnabled", provider.anthropic_cache_enabled, true);
    put_if_non_default(out, "anthropicCacheTtl", provider.anthropic_cache_ttl, "1h");
    return out;
}

void put_embedding_list(Value& object, std::string key,
                        const std::vector<EmbeddingModelConfig>& models) {
    Value array = array_value();
    for (const EmbeddingModelConfig& model : models) {
        array.elements.push_back(encode_embedding_model(model));
    }
    put_member(object, std::move(key), std::move(array));
}

void put_shell_list(Value& object, std::string key,
                    const std::vector<ShellDeviceConfig>& devices) {
    Value array = array_value();
    for (const ShellDeviceConfig& device : devices) {
        array.elements.push_back(encode_shell_device(device));
    }
    put_member(object, std::move(key), std::move(array));
}

void put_mcp_list(Value& object, std::string key, const std::vector<McpServerConfig>& servers) {
    Value array = array_value();
    for (const McpServerConfig& server : servers) array.elements.push_back(encode_mcp_server(server));
    put_member(object, std::move(key), std::move(array));
}

void put_provider_list(Value& object, std::string key,
                       const std::vector<CustomProviderConfig>& providers) {
    Value array = array_value();
    for (const CustomProviderConfig& provider : providers) {
        array.elements.push_back(encode_custom_provider(provider));
    }
    put_member(object, std::move(key), std::move(array));
}

void put_nullable_string(Value& object, std::string key, const std::optional<std::string>& value) {
    put_member(object, std::move(key), value.has_value() ? string_value(*value) : null_value());
}

void put_nullable_int(Value& object, std::string key, const std::optional<int>& value) {
    put_member(object, std::move(key), value.has_value() ? int_value(*value) : null_value());
}

void put_nullable_float(Value& object, std::string key, const std::optional<float>& value) {
    put_member(object, std::move(key), value.has_value() ? float_value(*value) : null_value());
}

}  // namespace

std::string CustomProviderConfig::provider_id() const {
    return provider_identity::is_stable_id(id) ? id : name;
}

json::Value to_json_object(const SettingsSnapshot& s, bool custom_font_included) {
    Value out = object_value();
    put_member(out, "selectedModel", string_value(s.selected_model));
    put_string_array(out, "customModels", s.custom_models);
    put_string_array(out, "enabledModels", s.enabled_models);
    put_string_map(out, "modelAliases", s.model_aliases);
    put_bool_map(out, "modelProviderNames", s.model_provider_names);
    put_member(out, "contextTokenBudget", int_value(s.max_context_window));
    put_member(out, "visualizeContextRollout", bool_value(s.visualize_context_rollout));
    put_member(out, "contextCompactEnabled", bool_value(s.context_compact_enabled));
    put_nullable_string(out, "contextCompactModel", s.context_compact_model);
    put_member(out, "contextCompactPrompt", string_value(s.context_compact_prompt));
    put_member(out, "contextCompactRetainCount", int_value(s.context_compact_retain_count));
    put_member(out, "contextCompactPreserveSystemPrompt",
               bool_value(s.context_compact_preserve_system_prompt));
    put_member(out, "contextCompactThresholdPercent", int_value(s.context_compact_threshold_percent));
    put_member(out, "codeExecutionEnabled", bool_value(s.code_execution_enabled));
    put_member(out, "googleSearchEnabled", bool_value(s.google_search_enabled));
    put_member(out, "thinkingEnabled", bool_value(s.thinking_enabled));
    put_member(out, "thinkingLevel", string_value(s.thinking_level));
    put_member(out, "thinkingBudgetEnabled", bool_value(s.thinking_budget_enabled));
    put_member(out, "thinkingBudgetTokens", int_value(s.thinking_budget_tokens));
    put_member(out, "openAiServiceTierEnabled", bool_value(s.open_ai_service_tier_enabled));
    put_member(out, "openAiServiceTier", string_value(s.open_ai_service_tier));
    put_member(out, "openAiResponsesApiEnabled", bool_value(s.open_ai_responses_api_enabled));
    put_member(out, "anthropicCacheEnabled", bool_value(s.anthropic_cache_enabled));
    put_member(out, "anthropicCacheTtl", string_value(s.anthropic_cache_ttl));
    put_string_map(out, "providerBaseUrls", s.provider_base_urls);
    put_member(out, "titleGenerationEnabled", bool_value(s.title_generation_enabled));
    put_nullable_string(out, "titleGenerationModel", s.title_generation_model);
    put_member(out, "titleGenerationPrompt", string_value(s.title_generation_prompt));
    put_member(out, "titleGenerationNotificationsEnabled",
               bool_value(s.title_generation_notifications_enabled));
    put_member(out, "accessPastConversations", bool_value(s.access_past_conversations));
    put_member(out, "accessSavedMemories", bool_value(s.access_saved_memories));
    put_member(out, "accessActiveMemory", bool_value(s.access_active_memory));
    put_member(out, "accessSkills", bool_value(s.access_skills));
    put_member(out, "ragSearchEnabled", bool_value(s.rag_search_enabled));
    put_member(out, "modelSearchMethod", string_value(s.model_search_method));
    put_member(out, "manualSearchMethod", string_value(s.manual_search_method));

    std::vector<EmbeddingModelConfig> remote_models;
    for (const EmbeddingModelConfig& model : s.embedding_models) {
        std::optional<EmbeddingModelConfig> remote =
            native_backup::as_portable_remote_config(model);
        if (remote.has_value()) remote_models.push_back(std::move(*remote));
    }
    put_embedding_list(out, "remoteEmbeddingModels", remote_models);
    const bool active_is_remote =
        std::any_of(remote_models.begin(), remote_models.end(),
                    [&](const EmbeddingModelConfig& model) {
                        return model.id == s.active_embedding_model_id;
                    });
    put_nullable_string(out, "activeRemoteEmbeddingModelId",
                        active_is_remote && !s.active_embedding_model_id.empty()
                            ? std::optional<std::string>(s.active_embedding_model_id)
                            : std::nullopt);

    put_member(out, "appLanguage", string_value(s.app_language));
    put_member(out, "webSearchEnabled", bool_value(s.web_search_enabled));
    put_member(out, "webSearchProvider", string_value(s.web_search_provider));
    put_member(out, "webSearchNumResults", int_value(s.web_search_num_results));
    put_member(out, "webSearchBaseUrl", string_value(s.web_search_base_url));
    put_member(out, "imageGenEnabled", bool_value(s.image_gen_enabled));
    put_nullable_string(out, "imageGenModel", s.image_gen_model);
    put_member(out, "imageGenSize", string_value(s.image_gen_size));
    put_member(out, "searchContextWindow", int_value(s.search_context_window));
    put_member(out, "searchMatchLimit", int_value(s.search_match_limit));
    put_member(out, "ragThreshold", float_value(s.rag_threshold));
    put_member(out, "autoCacheEnabled", bool_value(s.auto_cache_enabled));
    put_member(out, "showUncachedNotification", bool_value(s.show_uncached_notification));
    put_member(out, "autoUpdateCheck", bool_value(s.auto_update_check));

    put_member(out, "imageTranscriptionEnabled", bool_value(s.image_transcription_enabled));
    put_string_array(out, "imageTranscriptionEnabledModels",
                     s.image_transcription_enabled_models);
    put_nullable_string(out, "imageTranscriptionModel", s.image_transcription_model);
    put_member(out, "imageTranscriptionBatchSize", int_value(s.image_transcription_batch_size));
    put_member(out, "imageTranscriptionPrompt", string_value(s.image_transcription_prompt));

    put_member(out, "shellEnabled", bool_value(s.shell_enabled));
    put_member(out, "shellConfirmEnabled", bool_value(s.shell_confirm_enabled));
    put_member(out, "askUserEnabled", bool_value(s.ask_user_enabled));
    std::vector<ShellDeviceConfig> shell_without_secrets;
    for (const ShellDeviceConfig& device : s.shell_devices) {
        shell_without_secrets.push_back(native_backup::without_secrets(device));
    }
    put_shell_list(out, "shellDevices", shell_without_secrets);
    put_member(out, "automationToolsEnabled", bool_value(s.automation_tools_enabled));
    put_member(out, "exactExecutionEnabled", bool_value(s.exact_execution_enabled));
    put_member(out, "automationWakeLockEnabled", bool_value(s.automation_wake_lock_enabled));
    put_provider_list(out, "customProviders", s.custom_providers);
    std::vector<McpServerConfig> mcp_without_secrets;
    for (const McpServerConfig& server : s.mcp_servers) {
        mcp_without_secrets.push_back(native_backup::without_secrets(server));
    }
    put_mcp_list(out, "mcpServers", mcp_without_secrets);

    put_member(out, "proxyEnabled", bool_value(s.proxy_enabled));
    put_member(out, "proxyType", string_value(s.proxy_type));
    put_member(out, "proxyHost", string_value(s.proxy_host));
    put_member(out, "proxyPort", string_value(s.proxy_port));
    put_member(out, "proxyUsername", string_value(s.proxy_username));
    put_member(out, "proxyBypass", string_value(s.proxy_bypass));

    put_member(out, "showDocumentationFab", bool_value(s.show_documentation_fab));
    put_member(out, "themeMode", string_value(s.theme_mode));
    put_member(out, "amoledEnabled", bool_value(s.amoled_enabled));
    put_member(out, "colorScheme", string_value(s.color_scheme));
    put_member(out, "dynamicColor", bool_value(s.dynamic_color));
    put_member(out, "blurEffectsEnabled", bool_value(s.blur_effects_enabled));
    put_member(out, "reduceMotion", bool_value(s.reduce_motion));
    put_member(out, "stickToBottom", bool_value(s.stick_to_bottom));
    put_member(out, "parseInlineDollarMath", bool_value(s.parse_inline_dollar_math));
    put_member(out, "autoWrapCodeBlocks", bool_value(s.auto_wrap_code_blocks));
    put_member(out, "hapticsEnabled", bool_value(s.haptics_enabled));
    put_member(out, "detailedTokenUsage", bool_value(s.detailed_token_usage));
    put_member(out, "toolCallDisplayMode", string_value(s.tool_call_display_mode));
    put_member(out, "thinkingSegmentDisplayMode", string_value(s.thinking_segment_display_mode));
    put_member(out, "autoExpandActiveGroup", bool_value(s.auto_expand_active_group));
    put_member(out, "schemeStyle", string_value(s.scheme_style));

    const std::string font_preference =
        (s.font_preference == "custom" && !custom_font_included) ? "app_default"
                                                                : s.font_preference;
    put_member(out, "fontPreference", string_value(font_preference));
    if (custom_font_included) {
        put_member(out, "customFontName", string_value(s.custom_font_name));
    }

    put_nullable_float(out, "defaultTemperature", s.default_temperature);
    put_nullable_int(out, "defaultMaxTokens", s.default_max_tokens);
    put_nullable_float(out, "defaultTopP", s.default_top_p);
    put_nullable_float(out, "defaultFrequencyPenalty", s.default_frequency_penalty);
    put_nullable_float(out, "defaultPresencePenalty", s.default_presence_penalty);
    put_nullable_string(out, "activeSystemPromptId", s.active_system_prompt_id);
    return out;
}

RestoreResult restore_from_json_object(const json::Value& obj, const SettingsSnapshot& current,
                                       SettingsSink& sink, const RestoreOptions& options) {
    if (obj.kind != Kind::Object) {
        throw IllegalArgumentException("settings payload is not a JsonObject");
    }
    RestoreResult result;
    std::vector<std::string>& warnings = result.warnings;

    const std::vector<ShellDeviceConfig>& previous_shell_devices = current.shell_devices;
    const std::vector<McpServerConfig>& previous_mcp_servers = current.mcp_servers;
    const std::vector<EmbeddingModelConfig>& previous_embedding_models = current.embedding_models;
    const std::string& previous_font_path = current.custom_font_path;
    const std::string& previous_font_name = current.custom_font_name;
    const std::vector<CustomProviderConfig>& previous_custom_providers = current.custom_providers;

    // cacheRecords = listOf(obj) + customProviders entries that are objects.
    std::vector<const Value*> cache_records = {&obj};
    if (const Value* providers = field(obj, "customProviders");
        providers != nullptr && providers->kind == Kind::Array) {
        for (const Value& element : providers->elements) {
            if (element.kind == Kind::Object) cache_records.push_back(&element);
        }
    }
    for (const Value* record : cache_records) {
        if (const Value* ttl = field(*record, "anthropicCacheTtl"); ttl != nullptr) {
            if (!(ttl->kind == Kind::String &&
                  (ttl->text == "5m" || ttl->text == "1h"))) {
                throw IllegalArgumentException("Invalid Anthropic cache duration");
            }
        }
        if (const Value* flag = field(*record, "anthropicCacheEnabled"); flag != nullptr) {
            // require(value is JsonPrimitive && !value.isString && value.booleanOrNull != null)
            if (!(flag->kind == Kind::Literal && json_boolean(flag).has_value())) {
                throw IllegalArgumentException("Invalid Anthropic cache switch");
            }
        }
    }

    const std::optional<std::string> imported_cache_ttl = string_field(obj, "anthropicCacheTtl");
    const std::optional<std::vector<CustomProviderConfig>> imported_raw_providers =
        decode_custom_provider_list(field(obj, "customProviders"));
    const ImportedProviderIdentities identities =
        prepare_imported_custom_providers(imported_raw_providers.value_or(
                                              std::vector<CustomProviderConfig>{}),
                                          previous_custom_providers, options.replace,
                                          options.provider_id_generator);
    if (!identities.rejected.empty()) {
        std::vector<std::string> rejected_names;
        for (const CustomProviderConfig& config : identities.rejected) {
            rejected_names.push_back(config.name);
        }
        warnings.push_back("skipped invalid custom provider name(s): " +
                           join_names(rejected_names));
    }
    const auto remap_model = [&](const std::string& model_id) {
        return provider_identity::remap_provider_reference(model_id,
                                                           identities.model_reference_remap);
    };

    if (options.replace) sink.reset_portable_settings_for_import();

    if (const std::optional<bool> flag = boolean_field(obj, "anthropicCacheEnabled")) {
        sink.save_anthropic_cache_enabled(*flag);
    }
    if (imported_cache_ttl.has_value()) sink.save_anthropic_cache_ttl(*imported_cache_ttl);

    if (const std::optional<std::string> model = string_field(obj, "selectedModel")) {
        sink.save_selected_model(remap_model(*model));
    }

    if (const auto imported = decode_string_set(field(obj, "customModels"))) {
        std::vector<std::string> remapped;
        for (const std::string& model : *imported) remapped.push_back(remap_model(model));
        sink.save_custom_models(options.replace ? remapped
                                                : merge_sets(current.custom_models, remapped));
    }
    if (const auto imported = decode_string_set(field(obj, "enabledModels"))) {
        std::vector<std::string> remapped;
        for (const std::string& model : *imported) remapped.push_back(remap_model(model));
        sink.save_enabled_models(options.replace ? remapped
                                                 : merge_sets(current.enabled_models, remapped));
    }
    if (const auto imported = decode_string_map(field(obj, "modelAliases"))) {
        StringMap remapped;
        for (const auto& entry : *imported) {
            put_or_assign(remapped, remap_model(entry.first), entry.second);
        }
        sink.save_model_aliases(options.replace ? remapped
                                                : merge_maps(current.model_aliases, remapped));
    }
    if (const auto imported = decode_bool_map(field(obj, "modelProviderNames"))) {
        // Kotlin remapModelPreferenceKeys is generic over the value type; the String
        // specialization is pinned by the provider-identity stand, this is the same two-pass
        // body over Map<String, Boolean>.
        BoolMap result;
        for (const auto& entry : *imported) {
            if (provider_identity::remap_provider_reference(
                    entry.first, identities.model_reference_remap) == entry.first) {
                put_or_assign(result, entry.first, entry.second);
            }
        }
        for (const auto& entry : *imported) {
            const std::string remapped = provider_identity::remap_provider_reference(
                entry.first, identities.model_reference_remap);
            if (remapped != entry.first) {
                bool exists = false;
                for (const auto& existing : result) {
                    if (existing.first == remapped) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) result.emplace_back(remapped, entry.second);
            }
        }
        sink.save_model_provider_names(result, options.replace);
    }

    const std::optional<int> context_window =
        int_field(obj, "contextTokenBudget").has_value()
            ? int_field(obj, "contextTokenBudget")
            : int_field(obj, "maxContextWindow");
    if (context_window.has_value()) sink.save_max_context_window(*context_window);
    if (const std::optional<bool> flag = boolean_field(obj, "visualizeContextRollout")) {
        sink.save_visualize_context_rollout(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "contextCompactEnabled")) {
        sink.save_context_compact_enabled(*flag);
    }
    if (has_field(obj, "contextCompactModel")) {
        const std::optional<std::string> value = nullable_string_field(obj, "contextCompactModel");
        sink.save_context_compact_model(value.has_value()
                                            ? std::optional<std::string>(remap_model(*value))
                                            : std::nullopt);
    }
    if (const std::optional<std::string> value = string_field(obj, "contextCompactPrompt")) {
        sink.save_context_compact_prompt(*value);
    }
    if (const std::optional<int> count = int_field(obj, "contextCompactRetainCount");
        count.has_value() && *count >= 0) {
        sink.save_context_compact_retain_count(*count);
    }
    if (const std::optional<bool> flag =
            boolean_field(obj, "contextCompactPreserveSystemPrompt")) {
        sink.save_context_compact_preserve_system_prompt(*flag);
    }
    if (const std::optional<int> percent =
            imported_context_compact_threshold(int_field(obj, "contextCompactThresholdPercent"))) {
        sink.save_context_compact_threshold_percent(*percent);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "codeExecutionEnabled")) {
        sink.save_code_execution_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "googleSearchEnabled")) {
        sink.save_google_search_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "thinkingEnabled")) {
        sink.save_thinking_enabled(*flag);
    }
    if (const std::optional<std::string> level = string_field(obj, "thinkingLevel")) {
        const std::optional<int> legacy_budget =
            agora::core::thinking_capability::legacy_budget_tokens(level);
        sink.save_thinking_level(agora::core::thinking_capability::normalize(level));
        if (!has_field(obj, "thinkingBudgetEnabled") && legacy_budget.has_value()) {
            sink.save_thinking_budget_enabled(true);
        }
        if (!has_field(obj, "thinkingBudgetTokens") && legacy_budget.has_value()) {
            sink.save_thinking_budget_tokens(*legacy_budget);
        }
    }
    if (const std::optional<bool> flag = boolean_field(obj, "thinkingBudgetEnabled")) {
        sink.save_thinking_budget_enabled(*flag);
    }
    if (const std::optional<int> tokens = int_field(obj, "thinkingBudgetTokens")) {
        sink.save_thinking_budget_tokens(*tokens);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "openAiServiceTierEnabled")) {
        sink.save_open_ai_service_tier_enabled(*flag);
    }
    if (const std::optional<std::string> tier = string_field(obj, "openAiServiceTier")) {
        sink.save_open_ai_service_tier(agora::core::openai_service_tiers::normalize(tier));
    }
    if (const std::optional<bool> flag = boolean_field(obj, "openAiResponsesApiEnabled")) {
        sink.save_open_ai_responses_api_enabled(*flag);
    }

    if (const auto imported = decode_string_map(field(obj, "providerBaseUrls"))) {
        StringMap remapped;
        for (const auto& entry : *imported) {
            std::string key = entry.first;
            for (const auto& name_entry : identities.provider_name_remap) {
                if (name_entry.first == entry.first) {
                    key = name_entry.second;
                    break;
                }
            }
            put_or_assign(remapped, key, entry.second);
        }
        sink.save_provider_base_urls(options.replace
                                         ? remapped
                                         : merge_maps(current.provider_base_urls, remapped));
    }
    if (const std::optional<bool> flag = boolean_field(obj, "titleGenerationEnabled")) {
        sink.save_title_generation_enabled(*flag);
    }
    if (has_field(obj, "titleGenerationModel")) {
        const std::optional<std::string> value = nullable_string_field(obj, "titleGenerationModel");
        sink.save_title_generation_model(value.has_value()
                                             ? std::optional<std::string>(remap_model(*value))
                                             : std::nullopt);
    }
    if (const std::optional<std::string> value = string_field(obj, "titleGenerationPrompt")) {
        sink.save_title_generation_prompt(*value);
    }
    if (const std::optional<bool> flag =
            boolean_field(obj, "titleGenerationNotificationsEnabled")) {
        sink.save_title_generation_notifications_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "accessPastConversations")) {
        sink.save_access_past_conversations(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "accessSavedMemories")) {
        sink.save_access_saved_memories(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "accessActiveMemory")) {
        sink.save_access_active_memory(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "accessSkills")) {
        sink.save_access_skills(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "ragSearchEnabled")) {
        sink.save_rag_search_enabled(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "modelSearchMethod")) {
        sink.save_model_search_method(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "manualSearchMethod")) {
        sink.save_manual_search_method(*value);
    }

    // ---- remote embedding models -------------------------------------------------------
    const Value* remote_element = field(obj, "remoteEmbeddingModels");
    if (remote_element == nullptr) remote_element = field(obj, "embeddingModels");
    if (remote_element != nullptr || options.replace) {
        std::vector<EmbeddingModelConfig> decoded;
        if (remote_element != nullptr) {
            const auto parsed = decode_embedding_list(remote_element);
            if (parsed.has_value()) decoded = *parsed;
        }
        std::vector<EmbeddingModelConfig> imported_remote;
        for (const EmbeddingModelConfig& model : decoded) {
            std::optional<EmbeddingModelConfig> remote =
                native_backup::as_portable_remote_config(model);
            if (remote.has_value()) imported_remote.push_back(std::move(*remote));
        }
        std::vector<std::pair<std::string, const EmbeddingModelConfig*>> previous_by_id;
        for (const EmbeddingModelConfig& model : previous_embedding_models) {
            put_or_assign(previous_by_id, model.id, &model);
        }
        std::vector<EmbeddingModelConfig> restored_remote;
        for (const EmbeddingModelConfig& imported : imported_remote) {
            std::string legacy_key;
            if (options.allow_legacy_secrets && remote_element != nullptr) {
                const auto raw_again = decode_embedding_list(remote_element);
                if (raw_again.has_value()) {
                    for (const EmbeddingModelConfig& candidate : *raw_again) {
                        if (candidate.id == imported.id) {
                            legacy_key = candidate.remote_api_key;
                            break;
                        }
                    }
                }
            }
            EmbeddingModelConfig model = imported;
            if (legacy_key.empty()) {
                for (const auto& entry : previous_by_id) {
                    if (entry.first == imported.id) {
                        legacy_key = entry.second->remote_api_key;
                        break;
                    }
                }
            }
            model.remote_api_key = legacy_key;
            model.local_file_path = "";
            restored_remote.push_back(std::move(model));
        }
        std::vector<EmbeddingModelConfig> existing_local;
        for (const EmbeddingModelConfig& model : previous_embedding_models) {
            if (model.type == EmbeddingModelType::Local) existing_local.push_back(model);
        }
        std::vector<EmbeddingModelConfig> final_remote;
        if (options.replace) {
            final_remote = restored_remote;
        } else {
            std::vector<EmbeddingModelConfig> previous_remote;
            for (const EmbeddingModelConfig& model : previous_embedding_models) {
                if (model.type == EmbeddingModelType::Remote) previous_remote.push_back(model);
            }
            final_remote = merge_by_id<EmbeddingModelConfig, std::string>(
                previous_remote, restored_remote,
                [](const EmbeddingModelConfig& model) { return model.id; });
        }
        std::vector<EmbeddingModelConfig> final_models = existing_local;
        final_models.insert(final_models.end(), final_remote.begin(), final_remote.end());
        sink.save_embedding_models(final_models);

        std::optional<std::string> requested;
        if (has_field(obj, "activeRemoteEmbeddingModelId")) {
            requested = nullable_string_field(obj, "activeRemoteEmbeddingModelId");
        } else if (has_field(obj, "activeEmbeddingModelId")) {
            requested = string_field(obj, "activeEmbeddingModelId");
        }
        std::optional<std::string> active;
        if (requested.has_value()) {
            for (const EmbeddingModelConfig& model : final_models) {
                if (model.id == *requested) active = *requested;
            }
        }
        if (!active.has_value()) {
            for (const EmbeddingModelConfig& model : final_models) {
                if (model.id == current.active_embedding_model_id &&
                    model.type == EmbeddingModelType::Local) {
                    active = current.active_embedding_model_id;
                }
            }
        }
        sink.set_active_embedding_model_id(active.value_or(""));
    }

    if (const std::optional<std::string> value = string_field(obj, "appLanguage")) {
        sink.save_app_language(*value);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "webSearchEnabled")) {
        sink.save_web_search_enabled(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "webSearchProvider")) {
        sink.save_web_search_provider(*value);
    }
    if (const std::optional<int> value = int_field(obj, "webSearchNumResults")) {
        sink.save_web_search_num_results(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "webSearchBaseUrl")) {
        sink.save_web_search_base_url(*value);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "imageGenEnabled")) {
        sink.save_image_gen_enabled(*flag);
    }
    if (has_field(obj, "imageGenModel")) {
        const std::optional<std::string> value = nullable_string_field(obj, "imageGenModel");
        sink.save_image_gen_model(value.has_value()
                                      ? std::optional<std::string>(remap_model(*value))
                                      : std::nullopt);
    }
    if (const std::optional<std::string> value = string_field(obj, "imageGenSize")) {
        sink.save_image_gen_size(*value);
    }
    if (const std::optional<int> value = int_field(obj, "searchContextWindow")) {
        sink.save_search_context_window(*value);
    }
    if (const std::optional<int> value = int_field(obj, "searchMatchLimit")) {
        sink.save_search_match_limit(*value);
    }
    if (const std::optional<float> value = float_field(obj, "ragThreshold")) {
        sink.save_rag_threshold(*value);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "autoCacheEnabled")) {
        sink.save_auto_cache_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "showUncachedNotification")) {
        sink.save_show_uncached_notification(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "autoUpdateCheck")) {
        sink.save_auto_update_check(*flag);
    }

    if (const std::optional<bool> flag = boolean_field(obj, "imageTranscriptionEnabled")) {
        sink.save_image_transcription_enabled(*flag);
    }
    if (const auto imported = decode_string_set(field(obj, "imageTranscriptionEnabledModels"))) {
        std::vector<std::string> remapped;
        for (const std::string& model : *imported) remapped.push_back(remap_model(model));
        sink.save_image_transcription_enabled_models(
            options.replace ? remapped
                            : merge_sets(current.image_transcription_enabled_models, remapped));
    }
    if (has_field(obj, "imageTranscriptionModel")) {
        const std::optional<std::string> value =
            nullable_string_field(obj, "imageTranscriptionModel");
        sink.save_image_transcription_model(value.has_value()
                                                ? std::optional<std::string>(remap_model(*value))
                                                : std::nullopt);
    }
    if (const std::optional<int> value = int_field(obj, "imageTranscriptionBatchSize")) {
        sink.save_image_transcription_batch_size(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "imageTranscriptionPrompt")) {
        sink.save_image_transcription_prompt(*value);
    }

    if (const std::optional<bool> flag = boolean_field(obj, "shellEnabled")) {
        sink.save_shell_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "shellConfirmEnabled")) {
        sink.save_shell_confirm_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "askUserEnabled")) {
        sink.save_ask_user_enabled(*flag);
    }
    const Value* shell_element = field(obj, "shellDevices");
    if (shell_element != nullptr || options.replace) {
        std::vector<ShellDeviceConfig> decoded;
        if (shell_element != nullptr) {
            const auto parsed = decode_shell_list(shell_element);
            if (parsed.has_value()) decoded = *parsed;
        }
        std::vector<ShellDeviceConfig> imported;
        for (const ShellDeviceConfig& raw : decoded) {
            ShellDeviceConfig device = raw;
            const ShellDeviceConfig* existing = nullptr;
            for (const ShellDeviceConfig& candidate : previous_shell_devices) {
                if (candidate.id == raw.id) {
                    existing = &candidate;
                    break;
                }
            }
            if (!options.allow_legacy_secrets) {
                device.api_key = existing != nullptr ? existing->api_key : std::string();
                device.ssh_password = existing != nullptr ? existing->ssh_password : std::string();
            }
            imported.push_back(std::move(device));
        }
        if (options.replace) {
            sink.save_shell_devices(imported);
        } else {
            sink.save_shell_devices(merge_by_id<ShellDeviceConfig, std::string>(
                previous_shell_devices, imported,
                [](const ShellDeviceConfig& device) { return device.id; }));
        }
    }
    if (const std::optional<bool> flag = boolean_field(obj, "automationToolsEnabled")) {
        sink.save_automation_tools_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "exactExecutionEnabled")) {
        sink.save_exact_execution_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "automationWakeLockEnabled")) {
        sink.save_automation_wake_lock_enabled(*flag);
    }

    if (has_field(obj, "customProviders")) {
        if (options.replace) {
            sink.save_custom_providers(identities.providers);
        } else {
            sink.save_custom_providers(merge_by_id<CustomProviderConfig, std::string>(
                previous_custom_providers, identities.providers,
                [](const CustomProviderConfig& provider) { return provider.provider_id(); }));
        }
    }

    const Value* mcp_element = field(obj, "mcpServers");
    if (mcp_element != nullptr || options.replace) {
        std::vector<McpServerConfig> decoded;
        if (mcp_element != nullptr) {
            const auto parsed = decode_mcp_list(mcp_element);
            if (parsed.has_value()) decoded = *parsed;
        }
        std::vector<McpServerConfig> imported;
        for (const McpServerConfig& raw : decoded) {
            McpServerConfig server = raw;
            if (!options.allow_legacy_secrets) {
                const McpServerConfig* existing = nullptr;
                for (const McpServerConfig& candidate : previous_mcp_servers) {
                    if (candidate.id == raw.id) {
                        existing = &candidate;
                        break;
                    }
                }
                server.headers = existing != nullptr ? existing->headers : StringMap{};
            }
            imported.push_back(std::move(server));
        }
        if (options.replace) {
            sink.save_mcp_servers(imported);
        } else {
            sink.save_mcp_servers(merge_by_id<McpServerConfig, std::string>(
                previous_mcp_servers, imported,
                [](const McpServerConfig& server) { return server.id; }));
        }
    }

    if (const std::optional<bool> flag = boolean_field(obj, "proxyEnabled")) {
        sink.save_proxy_enabled(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "proxyType")) {
        sink.save_proxy_type(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "proxyHost")) {
        sink.save_proxy_host(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "proxyPort")) {
        sink.save_proxy_port(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "proxyUsername")) {
        sink.save_proxy_username(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "proxyBypass")) {
        sink.save_proxy_bypass(*value);
    }

    if (const std::optional<bool> flag = boolean_field(obj, "showDocumentationFab")) {
        sink.save_show_documentation_fab(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "themeMode")) {
        sink.save_theme_mode(*value);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "amoledEnabled")) {
        sink.save_amoled_enabled(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "colorScheme")) {
        sink.save_color_scheme(*value);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "dynamicColor")) {
        sink.save_dynamic_color(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "blurEffectsEnabled")) {
        sink.save_blur_effects_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "reduceMotion")) {
        sink.save_reduce_motion(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "stickToBottom")) {
        sink.save_stick_to_bottom(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "parseInlineDollarMath")) {
        sink.save_parse_inline_dollar_math(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "autoWrapCodeBlocks")) {
        sink.save_auto_wrap_code_blocks(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "hapticsEnabled")) {
        sink.save_haptics_enabled(*flag);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "detailedTokenUsage")) {
        sink.save_detailed_token_usage(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "toolCallDisplayMode")) {
        sink.save_tool_call_display_mode(*value);
    }
    if (const std::optional<std::string> value = string_field(obj, "thinkingSegmentDisplayMode")) {
        sink.save_thinking_segment_display_mode(*value);
    }
    if (const std::optional<bool> flag = boolean_field(obj, "autoExpandActiveGroup")) {
        sink.save_auto_expand_active_group(*flag);
    }
    if (const std::optional<std::string> value = string_field(obj, "schemeStyle")) {
        sink.save_scheme_style(*value);
    }

    if (has_field(obj, "fontPreference") || options.replace) {
        const std::optional<std::string> requested_value = string_field(obj, "fontPreference");
        const std::string requested = requested_value.value_or("app_default");
        if (options.restored_custom_font.has_value()) {
            sink.save_custom_font_path(options.restored_custom_font->path);
            sink.save_custom_font_name(options.restored_custom_font->display_name);
            sink.save_font_preference(requested);
        } else if (requested == "custom" && !json::is_blank(previous_font_path) &&
                   std::filesystem::is_regular_file(previous_font_path)) {
            sink.save_custom_font_path(previous_font_path);
            sink.save_custom_font_name(previous_font_name);
            sink.save_font_preference("custom");
        } else {
            if (options.replace) {
                sink.save_custom_font_path("");
                sink.save_custom_font_name("");
            }
            sink.save_font_preference(requested == "custom" ? "app_default" : requested);
        }
    }

    if (has_field(obj, "defaultTemperature")) {
        sink.save_default_temperature(float_field(obj, "defaultTemperature"));
    }
    if (has_field(obj, "defaultMaxTokens")) {
        sink.save_default_max_tokens(int_field(obj, "defaultMaxTokens"));
    }
    if (has_field(obj, "defaultTopP")) {
        sink.save_default_top_p(float_field(obj, "defaultTopP"));
    }
    if (has_field(obj, "defaultFrequencyPenalty")) {
        sink.save_default_frequency_penalty(float_field(obj, "defaultFrequencyPenalty"));
    }
    if (has_field(obj, "defaultPresencePenalty")) {
        sink.save_default_presence_penalty(float_field(obj, "defaultPresencePenalty"));
    }

    const char* active_prompt_key = nullptr;
    if (has_field(obj, "activeSystemPromptId")) {
        active_prompt_key = "activeSystemPromptId";
    } else if (has_field(obj, "active_system_prompt_id")) {
        active_prompt_key = "active_system_prompt_id";
    }
    if (active_prompt_key != nullptr) {
        sink.set_active_system_prompt_id(
            options.resolve_system_prompt_id
                ? options.resolve_system_prompt_id(
                      nullable_string_field(obj, active_prompt_key))
                : std::nullopt);
    } else if (options.replace) {
        sink.set_active_system_prompt_id(std::nullopt);
    }

    sink.invalidate_portable_model_caches();
    return result;
}

}  // namespace agora::core::settings_archive
