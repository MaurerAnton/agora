#include "agora/core/provider_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::provider_identity {
namespace {

// ---------------------------------------------------------------------------------------------
// MD5 (RFC 1321)
//
// UUID.nameUUIDFromBytes is MD5-based, so the legacy id cannot be reproduced without it. Kept
// local to this translation unit rather than added to a shared crypto header: it exists for exactly
// one caller, and a half-used "crypto" module invites misuse. SHA-256 lives in its own file because
// the tool-call fingerprinting needs it.
// ---------------------------------------------------------------------------------------------

constexpr std::uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr std::uint32_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

inline std::uint32_t rotl(std::uint32_t value, std::uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

void md5_block(std::array<std::uint32_t, 4>& state, const std::uint8_t* block) {
    std::uint32_t words[16];
    for (int i = 0; i < 16; ++i) {
        words[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        int g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + kMd5K[i] + words[g], kMd5Shift[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::array<std::uint8_t, 16> md5(std::string_view data) {
    std::array<std::uint32_t, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

    const std::size_t full_blocks = data.size() / 64;
    for (std::size_t block = 0; block < full_blocks; ++block) {
        md5_block(state, reinterpret_cast<const std::uint8_t*>(data.data()) + block * 64);
    }

    // Tail: the remaining bytes, a 0x80 marker, zero padding to 56 mod 64, then the bit length.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - full_blocks * 64;
    for (std::size_t i = 0; i < rest; ++i) {
        tail[i] = static_cast<std::uint8_t>(data[full_blocks * 64 + i]);
    }
    tail[rest] = 0x80;
    const std::size_t tail_size = rest + 1 <= 56 ? 64 : 128;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_size - 8 + i] = static_cast<std::uint8_t>((bit_length >> (8 * i)) & 0xff);
    }
    md5_block(state, tail.data());
    if (tail_size == 128) md5_block(state, tail.data() + 64);

    std::array<std::uint8_t, 16> digest{};
    for (int i = 0; i < 4; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] & 0xff);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((state[i] >> 24) & 0xff);
    }
    return digest;
}

// ---------------------------------------------------------------------------------------------
// Stable ids
// ---------------------------------------------------------------------------------------------

constexpr std::string_view kPrefix = "custom-provider-";
constexpr char kHexLower[] = "0123456789abcdef";

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

std::string format_uuid(const std::array<std::uint8_t, 16>& bytes) {
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHexLower[bytes[i] >> 4]);
        out.push_back(kHexLower[bytes[i] & 0x0f]);
    }
    return out;
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

    std::array<std::uint8_t, 16> digest = md5(material);
    // UUID v3: overwrite the version and variant nibbles the way java.util.UUID does.
    digest[6] = static_cast<std::uint8_t>((digest[6] & 0x0f) | 0x30);
    digest[8] = static_cast<std::uint8_t>((digest[8] & 0x3f) | 0x80);
    return std::string(kPrefix) + format_uuid(digest);
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
