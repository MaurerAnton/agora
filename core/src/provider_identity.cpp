#include "agora/core/provider_identity.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/uuid_v3.hpp"

namespace agora::core::provider_identity {
namespace {

// ---------------------------------------------------------------------------------------------
// Stable ids
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kPrefix = "custom-provider-";

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/** ASCII-only fold, matching the boundary the other ports in this tree document. */
bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) return false;
    }
    return true;
}

/** Length of a stable id starting at `at`, or 0 when there is none. Case-insensitive. */
std::size_t stable_id_at(std::string_view text, std::size_t at) {
    if (text.size() - at < 52) return 0;
    if (!equals_ignore_case(text.substr(at, kPrefix.size()), kPrefix)) return 0;
    std::size_t cursor = at + kPrefix.size();

    for (int i = 0; i < 8; ++i) {
        if (!is_hex_digit(text[cursor + i])) return 0;
    }
    cursor += 8;
    if (text[cursor] != '-') return 0;
    ++cursor;
    for (int i = 0; i < 4; ++i) {
        if (!is_hex_digit(text[cursor + i])) return 0;
    }
    cursor += 4;
    if (text[cursor] != '-') return 0;
    ++cursor;
    // Version nibble: 1..5. A version 6 or 7 UUID is a different kind and must not be treated as a
    // custom provider id.
    if (text[cursor] < '1' || text[cursor] > '5') return 0;
    for (int i = 1; i < 4; ++i) {
        if (!is_hex_digit(text[cursor + i])) return 0;
    }
    cursor += 4;
    if (text[cursor] != '-') return 0;
    ++cursor;
    // Variant nibble: 8, 9, a or b.
    const char variant = ascii_lower(text[cursor]);
    if (variant != '8' && variant != '9' && variant != 'a' && variant != 'b') return 0;
    for (int i = 1; i < 4; ++i) {
        if (!is_hex_digit(text[cursor + i])) return 0;
    }
    cursor += 4;
    if (text[cursor] != '-') return 0;
    ++cursor;
    for (int i = 0; i < 12; ++i) {
        if (!is_hex_digit(text[cursor + i])) return 0;
    }
    return cursor + 12 - at;
}

const std::vector<std::string>& reserved_names() {
    static const std::vector<std::string> names = {
        "Google", "OpenAI", "Anthropic", "DeepSeek", "Qwen",
        "Groq",   "Ollama", "Open Router", "OpenCode Go", "Local"};
    return names;
}

bool contains(const std::vector<std::string>& values, const std::string& candidate) {
    for (const std::string& value : values) {
        if (value == candidate) return true;
    }
    return false;
}

/** LinkedHashSet add: keeps the first occurrence and its position. */
void add_unique(std::vector<std::string>& values, const std::string& candidate) {
    if (!contains(values, candidate)) values.push_back(candidate);
}

/** LinkedHashMap put: overwrites in place, appends on first insert. */
void map_put(ReferenceMap& map, const std::string& key, const std::string& value) {
    for (auto& entry : map) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    map.emplace_back(key, value);
}

void map_put_if_absent(ReferenceMap& map, const std::string& key, const std::string& value) {
    for (const auto& entry : map) {
        if (entry.first == key) return;
    }
    map.emplace_back(key, value);
}

}  // namespace

bool is_stable_id(std::string_view value) {
    const std::string trimmed = json::trim(value);
    return trimmed.size() == 52 && stable_id_at(trimmed, 0) == 52;
}

std::string legacy_id(std::string_view name) {
    std::string material = "agora/custom-provider/legacy/";
    const std::string normalized = json::trim(name);
    for (char c : normalized) material.push_back(ascii_lower(c));

    return std::string(kPrefix) + uuid_v3(material);
}

std::string replace_stable_ids_for_display(
    std::string_view text, const std::function<std::string(std::string_view)>& replacement) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t length = stable_id_at(text, at);
        if (length > 0) {
            out += replacement(text.substr(at, length));
            at += length;
        } else {
            out.push_back(text[at]);
            ++at;
        }
    }
    return out;
}

std::string provider_id(const Provider& provider) {
    // Kotlin: id.takeIf(::isStableId) ?: name -- the untrimmed id is what comes back.
    return is_stable_id(provider.id) ? provider.id : provider.name;
}

bool is_allowed_name(std::string_view name) {
    const std::string candidate = json::trim(name);
    if (candidate.empty()) return false;
    for (const std::string& reserved : reserved_names()) {
        if (equals_ignore_case(reserved, candidate)) return false;
    }
    return true;
}

bool has_conflict(std::string_view name, const std::vector<std::string>& existing_names,
                  std::optional<std::string> current_name) {
    const std::string candidate = json::trim(name);
    if (!is_allowed_name(candidate)) return true;
    for (const std::string& existing : existing_names) {
        const bool is_current_entry = current_name.has_value() && existing == *current_name;
        if (!is_current_entry && equals_ignore_case(json::trim(existing), candidate)) return true;
    }
    return false;
}

Sanitization sanitize(const std::vector<Provider>& configs) {
    Sanitization result;
    std::vector<std::string> accepted_names;
    for (const Provider& config : configs) {
        if (has_conflict(config.name, accepted_names)) {
            result.rejected.push_back(config);
        } else {
            result.accepted.push_back(config);
            accepted_names.push_back(config.name);
        }
    }
    return result;
}

Normalization normalize(const std::vector<Provider>& raw_providers,
                        const std::vector<std::string>& occupied_ids,
                        const IdGenerator& id_generator, const RandomIdSource& random_id) {
    const Sanitization sanitized = sanitize(raw_providers);

    // Kotlin filters with isStableId but keeps the original, untrimmed string.
    std::vector<std::string> used_ids;
    for (const std::string& id : occupied_ids) {
        if (is_stable_id(id)) add_unique(used_ids, id);
    }

    Normalization result;
    for (const Provider& raw : sanitized.accepted) {
        const std::string original_id = json::trim(raw.id);
        const bool retained = is_stable_id(original_id) && !contains(used_ids, original_id);

        std::string stable_id;
        if (retained) {
            stable_id = original_id;
        } else {
            bool allocated = false;
            for (int attempt = 0; attempt < 100 && !allocated; ++attempt) {
                // Only the first attempt is trimmed, matching newId(provider).trim() versus the
                // untrimmed policy newId() used for retries.
                std::string candidate =
                    attempt == 0 ? json::trim(id_generator(raw)) : random_id();
                if (is_stable_id(candidate) && !contains(used_ids, candidate)) {
                    stable_id = candidate;
                    allocated = true;
                }
            }
            if (!allocated) throw std::runtime_error("Unable to allocate a unique custom provider ID");
        }
        add_unique(used_ids, stable_id);

        std::vector<std::string> legacy_names;
        for (const std::string& name : raw.legacy_names) {
            add_unique(legacy_names, json::trim(name));
        }
        if (!retained) add_unique(legacy_names, json::trim(raw.name));
        // An empty legacy name is not a reference; Kotlin drops them before the filter below.
        std::vector<std::string> cleaned;
        for (const std::string& name : legacy_names) {
            if (!name.empty() && name != stable_id) cleaned.push_back(name);
        }

        Provider normalized;
        normalized.name = json::trim(raw.name);
        normalized.id = stable_id;
        normalized.legacy_names = cleaned;
        result.providers.push_back(normalized);
    }

    // distinct() over (legacyReference, providerId) pairs, first occurrence wins.
    for (const Provider& provider : result.providers) {
        for (const std::string& legacy : provider.legacy_names) {
            bool seen = false;
            for (const Migration& existing : result.migrations) {
                if (existing.legacy_reference == legacy && existing.provider_id == provider.id) {
                    seen = true;
                    break;
                }
            }
            if (!seen) result.migrations.push_back(Migration{legacy, provider.id});
        }
    }
    return result;
}

std::string remap_provider_reference(std::string_view reference, const ReferenceMap& migrations) {
    const std::pair<std::string, std::string>* best = nullptr;
    for (const auto& entry : migrations) {
        if (entry.first.empty()) continue;
        if (reference.size() <= entry.first.size()) continue;
        if (reference.compare(0, entry.first.size(), entry.first) != 0) continue;
        if (reference[entry.first.size()] != ':') continue;
        // maxByOrNull keeps the first maximum, so a later equal-length prefix must not win.
        if (best == nullptr || entry.first.size() > best->first.size()) best = &entry;
    }
    if (best == nullptr) return std::string(reference);
    return best->second + std::string(reference.substr(best->first.size()));
}

ReferenceMap remap_model_preference_keys(const ReferenceMap& aliases,
                                         const ReferenceMap& migrations) {
    ReferenceMap result;
    for (const auto& entry : aliases) {
        if (remap_provider_reference(entry.first, migrations) == entry.first) {
            map_put(result, entry.first, entry.second);
        }
    }
    for (const auto& entry : aliases) {
        const std::string remapped = remap_provider_reference(entry.first, migrations);
        if (remapped != entry.first) map_put_if_absent(result, remapped, entry.second);
    }
    return result;
}

std::string canonical_custom_model_id(std::string_view model_id,
                                      const std::vector<Provider>& custom_providers) {
    ReferenceMap migrations;
    for (const Provider& provider : custom_providers) {
        const std::string identity = provider_id(provider);
        for (const std::string& legacy : provider.legacy_names) {
            map_put(migrations, legacy, identity);
        }
        // The name is appended after the legacy names, so a provider whose name collides with an
        // earlier provider's legacy reference wins -- and an id collision keeps the first slot.
        map_put(migrations, provider.name, identity);
    }
    return remap_provider_reference(model_id, migrations);
}

}  // namespace agora::core::provider_identity
