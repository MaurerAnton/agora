#include "agora/core/model_aliases.hpp"

#include <cctype>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::aliases {
namespace {

namespace json = agora::core::json;

std::string lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    return out;
}

bool is_java_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\x0B' || c == '\f' || c == '\r';
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ascii_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_word_char(char c) { return is_ascii_letter(c) || is_digit(c); }

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() && lower_ascii(left) == lower_ascii(right);
}

bool starts_with_ignore_case(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

bool ends_with_ignore_case(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           equals_ignore_case(text.substr(text.size() - suffix.size()), suffix);
}

// -- display-name tables (insertion order matters for the numeric-suffix rule) --------------------

const std::vector<std::pair<std::string, std::string>>& exact_alias_tokens() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"a3b", "A3B"},   {"a70b", "A70B"}, {"e4b", "E4B"},   {"glm", "GLM"},
        {"mimo", "MiMo"}, {"minimax", "MiniMax"}, {"oss", "OSS"}, {"tts", "TTS"},
    };
    return table;
}

const std::vector<std::pair<std::string, std::string>>& alias_tokens() {
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"ai", "AI"},         {"api", "API"},       {"claude", "Claude"}, {"cohere", "Cohere"},
        {"command", "Command"}, {"deepseek", "DeepSeek"}, {"gemini", "Gemini"},
        {"gemma", "Gemma"},   {"gpt", "GPT"},       {"grok", "Grok"},     {"kimi", "Kimi"},
        {"llama", "Llama"},   {"llm", "LLM"},       {"mistral", "Mistral"},
        {"mixtral", "Mixtral"}, {"nova", "Nova"},   {"phi", "Phi"},       {"qwen", "Qwen"},
        {"qwq", "QwQ"},       {"vl", "VL"},         {"vlm", "VLM"},
    };
    return table;
}

const std::pair<std::string, std::string>* find_token(
    const std::vector<std::pair<std::string, std::string>>& table, const std::string& normalized) {
    for (auto& entry : table) {
        if (entry.first == normalized) return &entry;
    }
    return nullptr;
}

std::string humanize(std::string_view value);

// -- formatAliasToken --------------------------------------------------------------------------------

bool matches_version_token(const std::string& normalized) {  // [vr]\d+(?:\.\d+)?
    std::size_t i = (normalized[0] == 'v' || normalized[0] == 'V') ? 1 : 0;
    std::size_t digits = 0;
    while (i < normalized.size() && is_digit(normalized[i])) {
        ++i;
        ++digits;
    }
    if (digits == 0) return false;
    if (i == normalized.size()) return true;
    if (normalized[i] != '.') return false;
    ++i;
    std::size_t fraction = 0;
    while (i < normalized.size() && is_digit(normalized[i])) {
        ++i;
        ++fraction;
    }
    return fraction > 0 && i == normalized.size();
}

bool matches_o_series(const std::string& normalized) {  // o\d+(?:\.\d+)?
    if (normalized[0] != 'o') return false;
    std::size_t i = 1;
    std::size_t digits = 0;
    while (i < normalized.size() && is_digit(normalized[i])) {
        ++i;
        ++digits;
    }
    if (digits == 0) return false;
    if (i == normalized.size()) return true;
    if (normalized[i] != '.') return false;
    ++i;
    std::size_t fraction = 0;
    while (i < normalized.size() && is_digit(normalized[i])) {
        ++i;
        ++fraction;
    }
    return fraction > 0 && i == normalized.size();
}

bool matches_size_token(const std::string& normalized) {  // \d+(?:\.\d+)?[bkmt]
    if (normalized.empty()) return false;
    std::size_t i = normalized.size() - 1;
    const char unit = normalized[i];
    if (unit != 'b' && unit != 'k' && unit != 'm' && unit != 't') return false;
    std::string_view head(normalized.data(), normalized.size() - 1);
    if (head.empty() || !is_digit(head.front())) return false;
    std::size_t dot = head.find('.');
    if (dot == std::string_view::npos) {
        for (char c : head) {
            if (!is_digit(c)) return false;
        }
        return true;
    }
    if (dot == 0 || dot + 1 >= head.size()) return false;
    for (std::size_t index = 0; index < head.size(); ++index) {
        if (index == dot) continue;
        if (!is_digit(head[index])) return false;
    }
    return true;
}

std::string upper_first(const std::string& value) {
    if (value.empty()) return value;
    std::string out = value;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

std::string format_token(std::string_view value) {
    const std::string normalized = lower_ascii(value);
    if (const auto* exact = find_token(exact_alias_tokens(), normalized)) return exact->second;
    if (const auto* token = find_token(alias_tokens(), normalized)) return token->second;

    for (const auto& [token, display] : alias_tokens()) {
        if (normalized.size() > token.size() && normalized.compare(0, token.size(), token) == 0 &&
            is_digit(normalized[token.size()])) {
            const std::string numeric_suffix = normalized.substr(token.size());
            return token == "qwen" ? display + " " + numeric_suffix : display + numeric_suffix;
        }
    }
    if (matches_version_token(normalized)) return upper_first(normalized);
    if (matches_o_series(normalized)) return upper_first(normalized);
    if (matches_size_token(normalized)) {
        // `dropLast(1) + last().uppercaseChar()`
        std::string unit(1, normalized.back());
        if (unit[0] >= 'a' && unit[0] <= 'z') unit[0] = static_cast<char>(unit[0] - 'a' + 'A');
        return normalized.substr(0, normalized.size() - 1) + unit;
    }
    // replaceFirstChar { if (isLetter) titlecase(ROOT) else it.toString() } — ASCII boundary.
    return upper_first(normalized);
}

std::string humanize(std::string_view value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : value) {
        if (c == '-' || c == '_' || is_java_space(c)) {
            if (!current.empty()) tokens.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) tokens.push_back(current);

    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) out.push_back(' ');
        out += format_token(tokens[i]);
    }
    return out;
}

// -- the inferred families ------------------------------------------------------------------------------

/** `^(gemini-[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?)-preview$` (IGNORE_CASE); returns the slug. */
bool gemini_preview_slug(const std::string& slug, std::string* out) {
    if (!ends_with_ignore_case(slug, "-preview")) return false;
    const std::string head = slug.substr(0, slug.size() - std::string("-preview").size());
    if (head.size() < 8) return false;
    if (!starts_with_ignore_case(head, "gemini-")) return false;
    const char first = head[7];
    if (!is_word_char(first)) return false;
    if (head.size() == 8) {
        *out = head;
        return true;
    }
    const char last = head[head.size() - 1];
    if (!is_word_char(last)) return false;
    for (std::size_t i = 8; i + 1 < head.size(); ++i) {
        const char c = head[i];
        const bool allowed = is_word_char(c) || c == '.' || c == '_' || c == '-';
        if (!allowed) return false;
    }
    *out = head;
    return true;
}

/** The family part: `[a-z][a-z0-9]*` segments joined by '-'. */
bool is_claude_family(std::string_view family) {
    if (family.empty()) return false;
    std::size_t start = 0;
    while (start <= family.size()) {
        const std::size_t dash = family.find('-', start);
        const std::string_view segment = family.substr(
            start, dash == std::string_view::npos ? std::string_view::npos : dash - start);
        if (segment.empty() || !is_ascii_letter(segment[0])) return false;
        for (char c : segment) {
            if (!is_word_char(c)) return false;
        }
        if (dash == std::string_view::npos) break;
        start = dash + 1;
    }
    return true;
}

/** isValidSnapshotDate: LocalDate.parse(value, BASIC_ISO_DATE) — eight digits, real date. */
bool is_valid_snapshot_date(const std::string& value) {
    if (value.size() != 8) return false;
    for (char c : value) {
        if (!is_digit(c)) return false;
    }
    const int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(4, 2));
    const int day = std::stoi(value.substr(6, 2));
    if (month < 1 || month > 12) return false;
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = kDays[month - 1];
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        if (leap) limit = 29;
    }
    return day >= 1 && day <= limit;
}

struct ClaudeParts {
    std::string family;
    std::string major;
    std::string minor;
    std::string snapshot;
};

bool claude_family_first(const std::string& slug, ClaudeParts* parts) {
    if (!starts_with_ignore_case(slug, "claude-")) return false;
    std::string rest = slug.substr(std::string("claude-").size());

    for (int minor_mode = 0; minor_mode < 2; ++minor_mode) {
        for (int snapshot_mode = 0; snapshot_mode < 2; ++snapshot_mode) {
            for (int fast_mode = 0; fast_mode < 2; ++fast_mode) {
                std::string current = rest;
                ClaudeParts candidate;
                bool failed = false;

                if (fast_mode == 0) {
                    if (!ends_with_ignore_case(current, "-fast")) { failed = true; }
                    else { current.resize(current.size() - std::string("-fast").size()); }
                }
                if (!failed && snapshot_mode == 0) {
                    const std::string_view view(current);
                    if (view.size() < 9 || view[view.size() - 9] != '-') {
                        failed = true;
                    } else {
                        const std::string digits(view.substr(view.size() - 8));
                        bool numeric = true;
                        for (char c : digits) {
                            if (!is_digit(c)) numeric = false;
                        }
                        if (!numeric) {
                            failed = true;
                        } else {
                            candidate.snapshot = digits;
                            current.resize(current.size() - 9);
                        }
                    }
                }
                if (!failed && minor_mode == 0) {
                    const std::string_view view(current);
                    std::size_t digits = 0;
                    std::size_t position = view.size();
                    while (digits < 2 && position > 0 && is_digit(view[position - 1])) {
                        --position;
                        ++digits;
                    }
                    if (digits == 0 || position == 0 || view[position - 1] != '-') {
                        failed = true;
                    } else {
                        candidate.minor = std::string(view.substr(position, digits));
                        current.resize(position - 1);
                    }
                }
                if (failed) continue;
                const std::string_view view(current);
                std::size_t digits = 0;
                std::size_t position = view.size();
                while (digits < 2 && position > 0 && is_digit(view[position - 1])) {
                    --position;
                    ++digits;
                }
                if (digits == 0 || position == 0 || view[position - 1] != '-') continue;
                candidate.major = std::string(view.substr(position, digits));
                current.resize(position - 1);
                if (!is_claude_family(current)) continue;
                if (!candidate.snapshot.empty() && !is_valid_snapshot_date(candidate.snapshot)) {
                    return false;  // the pattern matched, but the date is rejected: no alias
                }
                candidate.family = current;
                *parts = candidate;
                return true;
            }
        }
    }
    return false;
}

bool claude_version_first(const std::string& slug, ClaudeParts* parts) {
    if (!starts_with_ignore_case(slug, "claude-")) return false;
    // NOTE: a string_view over a temporary std::string would dangle at the end of the statement.
    std::string_view rest(slug);
    rest.remove_prefix(std::string("claude-").size());

    std::size_t index = 0;
    std::size_t digits = 0;
    while (index < rest.size() && digits < 2 && is_digit(rest[index])) {
        ++index;
        ++digits;
    }
    if (digits == 0) return false;
    ClaudeParts candidate;
    candidate.major = std::string(rest.substr(0, digits));

    std::optional<std::string> minor;
    std::size_t position = index;
    if (index < rest.size() && rest[index] == '-') {
        std::size_t probe = index + 1;
        std::size_t minor_digits = 0;
        while (probe < rest.size() && minor_digits < 2 && is_digit(rest[probe])) {
            ++probe;
            ++minor_digits;
        }
        if (minor_digits > 0) {
            minor = std::string(rest.substr(index + 1, minor_digits));
            position = probe;
        }
    }
    if (minor) candidate.minor = *minor;
    // The family follows the (optional) minor through a '-'; the snapshot and the -fast suffix
    // hang off its end.
    if (position >= rest.size() || rest[position] != '-') return false;

    std::string tail(rest.substr(position + 1));
    for (int snapshot_mode = 0; snapshot_mode < 2; ++snapshot_mode) {
        for (int fast_mode = 0; fast_mode < 2; ++fast_mode) {
            std::string current = tail;
            std::string snapshot;
            bool failed = false;
            if (fast_mode == 0) {
                if (!ends_with_ignore_case(current, "-fast")) {
                    failed = true;
                } else {
                    current.resize(current.size() - std::string("-fast").size());
                }
            }
            if (!failed && snapshot_mode == 0) {
                const std::string_view view(current);
                if (view.size() < 9 || view[view.size() - 9] != '-') {
                    failed = true;
                } else {
                    const std::string digits(view.substr(view.size() - 8));
                    bool numeric = true;
                    for (char c : digits) {
                        if (!is_digit(c)) numeric = false;
                    }
                    if (!numeric) {
                        failed = true;
                    } else {
                        snapshot = digits;
                        current.resize(current.size() - 9);
                    }
                }
            }
            if (failed) continue;
            if (!is_claude_family(current)) continue;
            if (!snapshot.empty() && !is_valid_snapshot_date(snapshot)) return false;
            candidate.snapshot = snapshot;
            candidate.family = current;
            *parts = candidate;
            return true;
        }
    }
    return false;
}

std::string build_claude_alias(const ClaudeParts& parts, bool version_first) {
    std::string version = parts.major;
    if (!parts.minor.empty()) {
        version += ".";
        version += parts.minor;
    }
    const std::string family_name = humanize(parts.family);
    return version_first ? "Claude " + version + " " + family_name
                         : "Claude " + family_name + " " + version;
}

/** `^nova-(\d+(?:\.\d+)?)-(.+)-v\d+(?::\d+)?$` (IGNORE_CASE); the middle is greedy. */
bool nova_alias(const std::string& slug, std::string* out) {
    if (!starts_with_ignore_case(slug, "nova-")) return false;
    std::size_t index = std::string("nova-").size();
    std::size_t digits = 0;
    while (index < slug.size() && is_digit(slug[index])) {
        ++index;
        ++digits;
    }
    if (digits == 0) return false;
    if (index < slug.size() && slug[index] == '.') {
        ++index;
        std::size_t fraction = 0;
        while (index < slug.size() && is_digit(slug[index])) {
            ++index;
            ++fraction;
        }
        if (fraction == 0) return false;
    }
    std::string version = slug.substr(std::string("nova-").size(), index - std::string("nova-").size());
    if (index >= slug.size() || slug[index] != '-') return false;

    const std::string rest = slug.substr(index + 1);
    // Greedy `.+` takes the LAST "-v<digits>" that leaves a non-empty middle.
    for (std::size_t marker = rest.size(); marker > 0; --marker) {
        const std::size_t position = marker - 1;
        if (position < 2 || rest[position - 1] != '-' ||
            (rest[position] != 'v' && rest[position] != 'V')) {
            continue;
        }
        const std::string tail = rest.substr(position + 1);
        std::size_t digits_after = 0;
        std::size_t cursor = 0;
        while (cursor < tail.size() && is_digit(tail[cursor])) {
            ++cursor;
            ++digits_after;
        }
        if (digits_after == 0) continue;
        std::string suffix = tail.substr(cursor);
        if (!suffix.empty()) {
            if (suffix[0] != ':') return false;  // only ":<digits>" may follow
            for (std::size_t i = 1; i < suffix.size(); ++i) {
                if (!is_digit(suffix[i])) return false;
            }
        }
        const std::string middle = rest.substr(0, position - 1);
        if (middle.empty()) continue;
        *out = "Nova " + version + " " + humanize(middle);
        return true;
    }
    return false;
}

}  // namespace

std::string infer_model_alias(std::string_view model_name) {
    const std::string trimmed(json::trim(model_name));
    if (trimmed.empty()) return trimmed;

    const std::size_t slash = trimmed.rfind('/');
    std::string path_tail = trimmed.substr(slash == std::string::npos ? 0 : slash + 1);
    path_tail = json::trim(path_tail);
    if (path_tail.empty()) path_tail = trimmed;

    std::string without_variant = path_tail;
    if (ends_with_ignore_case(without_variant, ":batch") ||
        ends_with_ignore_case(without_variant, ":free")) {
        const std::size_t cut = without_variant.rfind(':');
        without_variant.resize(cut);
    }
    if (without_variant.empty()) without_variant = path_tail;

    std::string gemini_slug;
    if (gemini_preview_slug(without_variant, &gemini_slug)) return humanize(gemini_slug);

    ClaudeParts claude;
    if (claude_family_first(without_variant, &claude)) {
        return build_claude_alias(claude, false);
    }
    if (claude_version_first(without_variant, &claude)) {
        return build_claude_alias(claude, true);
    }

    std::string nova;
    if (nova_alias(without_variant, &nova)) return nova;

    const std::string humanized = humanize(without_variant);
    return humanized.empty() ? trimmed : humanized;
}

}  // namespace agora::core::aliases