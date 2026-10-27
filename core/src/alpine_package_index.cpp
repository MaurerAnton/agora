#include "agora/core/alpine_package_index.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>

#include "agora/core/zip_reader.hpp"

namespace agora::core::sandbox {

namespace {

/** Kotlin toIntOrNull: whole string must be an int (optional leading sign), else nullopt. */
std::optional<int> kotlin_int(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::string_view view(text);
    bool negative = false;
    if (view.front() == '+' || view.front() == '-') {
        negative = view.front() == '-';
        view.remove_prefix(1);
    }
    if (view.empty()) return std::nullopt;
    long long value = 0;
    const auto [end, ec] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (ec != std::errc{} || end != view.data() + view.size()) return std::nullopt;
    const long long limit = negative ? 2147483648LL : 2147483647LL;
    if (value > limit) return std::nullopt;  // Kotlin toIntOrNull returns null on overflow
    return negative ? static_cast<int>(-value) : static_cast<int>(value);
}

/** splitVersion: base before the last "-r", revision after it (0 when absent/unparsable). */
std::pair<std::string, int> split_version(const std::string& version) {
    const auto rev_index = version.rfind("-r");
    if (rev_index == std::string::npos) return {version, 0};
    return {version.substr(0, rev_index),
            kotlin_int(version.substr(rev_index + 2)).value_or(0)};
}

/** tokenise: '.', '_', '-' as single tokens; digit runs; letter runs. */
std::vector<std::string> tokenise(const std::string& version) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < version.size()) {
        const char c = version[i];
        if (c == '.' || c == '_' || c == '-') {
            tokens.emplace_back(1, c);
            ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = i;
            while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) ++i;
            tokens.push_back(version.substr(start, i - start));
        } else {
            const std::size_t start = i;
            while (i < version.size() && !std::isdigit(static_cast<unsigned char>(version[i])) &&
                   version[i] != '.' && version[i] != '_' && version[i] != '-') {
                ++i;
            }
            tokens.push_back(version.substr(start, i - start));
        }
    }
    return tokens;
}

int token_weight(const std::string& token) {
    if (token == "~") return -1;
    if (token.rfind("alpha", 0) == 0) return -4;
    if (token.rfind("beta", 0) == 0) return -3;
    if (token.rfind("pre", 0) == 0) return -2;
    if (token.rfind("rc", 0) == 0) return -1;
    return 0;
}

int sign_of(int value) { return value < 0 ? -1 : (value == 0 ? 0 : 1); }

/** compareToken: numeric when both parse, else pre-release weight, else lexicographic. */
int compare_token(const std::string& a, const std::string& b) {
    const std::optional<int> a_digits = kotlin_int(a);
    const std::optional<int> b_digits = kotlin_int(b);
    if (a_digits && b_digits) {
        if (*a_digits != *b_digits) return *a_digits < *b_digits ? -1 : 1;
        return 0;
    }
    const int weight_a = token_weight(a);
    const int weight_b = token_weight(b);
    if (weight_a != 0 || weight_b != 0) return sign_of(weight_a - weight_b);
    const int lex = a.compare(b);
    return lex < 0 ? -1 : (lex == 0 ? 0 : 1);
}

std::string trim_copy(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string nul_terminated(const char* data, std::size_t size) {
    std::string out(data, data + size);
    const auto nul = out.find('\0');
    if (nul != std::string::npos) out.resize(nul);
    return out;
}

// ── gzip (RFC 1952) + tar ────────────────────────────────────────────────────────

/** Gunzip a complete gzip member: header flags, raw inflate, CRC32/ISIZE verification. */
std::string gunzip(const std::string& bytes) {
    if (bytes.size() < 18) throw std::runtime_error("gzip: stream too short");
    if (static_cast<unsigned char>(bytes[0]) != 0x1F ||
        static_cast<unsigned char>(bytes[1]) != 0x8B) {
        throw std::runtime_error("gzip: bad magic");
    }
    if (bytes[2] != 0x08) throw std::runtime_error("gzip: unsupported compression method");
    const auto flags = static_cast<unsigned char>(bytes[3]);
    if ((flags & 0xE0) != 0) throw std::runtime_error("gzip: reserved flags set");
    std::size_t p = 10;
    const auto need = [&](std::size_t extra) {
        if (p + extra > bytes.size()) throw std::runtime_error("gzip: truncated header");
    };
    if (flags & 0x04) {  // FEXTRA
        need(2);
        const auto xlen = static_cast<unsigned char>(bytes[p]) |
                          (static_cast<unsigned char>(bytes[p + 1]) << 8);
        p += 2;
        need(xlen);
        p += xlen;
    }
    if (flags & 0x08) {  // FNAME
        const auto nul = bytes.find('\0', p);
        if (nul == std::string::npos) throw std::runtime_error("gzip: truncated FNAME");
        p = nul + 1;
    }
    if (flags & 0x10) {  // FCOMMENT
        const auto nul = bytes.find('\0', p);
        if (nul == std::string::npos) throw std::runtime_error("gzip: truncated FCOMMENT");
        p = nul + 1;
    }
    if (flags & 0x02) {  // FHCRC
        need(2);
        p += 2;
    }
    if (p >= bytes.size()) throw std::runtime_error("gzip: no deflate payload");
    std::size_t consumed = 0;
    std::string out = zip::inflate_raw_stream(bytes.substr(p), &consumed);
    const std::size_t trailer = p + consumed;
    if (trailer + 8 > bytes.size()) throw std::runtime_error("gzip: truncated trailer");
    const auto byte_at = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    const std::uint32_t expected_crc = byte_at(trailer) | (byte_at(trailer + 1) << 8) |
                                       (byte_at(trailer + 2) << 16) | (byte_at(trailer + 3) << 24);
    const std::uint32_t expected_size = byte_at(trailer + 4) | (byte_at(trailer + 5) << 8) |
                                        (byte_at(trailer + 6) << 16) | (byte_at(trailer + 7) << 24);
    if (zip::crc32(out) != expected_crc) throw std::runtime_error("gzip: CRC32 mismatch");
    if ((out.size() & 0xFFFFFFFFu) != expected_size) {
        throw std::runtime_error("gzip: ISIZE mismatch");
    }
    return out;
}

/** Parse an octal tar size field (NUL/space padded); throws on non-octal input. */
std::size_t tar_octal_size(const char* field, std::size_t size) {
    std::size_t value = 0;
    bool seen_digit = false;
    for (std::size_t i = 0; i < size; ++i) {
        const char c = field[i];
        if (c == '\0' || c == ' ') {
            if (seen_digit) break;
            continue;
        }
        if (c < '0' || c > '7') throw std::runtime_error("tar: unsupported size field");
        seen_digit = true;
        value = value * 8 + static_cast<std::size_t>(c - '0');
    }
    return value;
}

/** Every `APKINDEX` member of an uncompressed ustar/GNU tar (long names supported). */
std::vector<std::string> apkindex_members(const std::string& tar) {
    std::vector<std::string> members;
    std::string pending_long_name;
    std::size_t offset = 0;
    while (offset + 512 <= tar.size()) {
        const char* block = tar.data() + offset;
        bool all_zero = true;
        for (int i = 0; i < 512; ++i) {
            if (block[i] != '\0') { all_zero = false; break; }
        }
        if (all_zero) break;  // end-of-archive marker

        const char typeflag = block[156];
        if (typeflag == 'L') {  // GNU long name: payload is the next block
            const std::size_t name_size = tar_octal_size(block + 124, 12);
            if (offset + 512 + name_size > tar.size()) {
                throw std::runtime_error("tar: truncated long name");
            }
            pending_long_name = nul_terminated(tar.data() + offset + 512, name_size);
            offset += 512 + ((name_size + 511) / 512) * 512;
            continue;
        }

        std::string entry_name;
        if (!pending_long_name.empty()) {
            entry_name = pending_long_name;
            pending_long_name.clear();
        } else {
            entry_name = nul_terminated(block, 100);
            const std::string prefix = nul_terminated(block + 345, 155);
            if (!prefix.empty()) entry_name = prefix + "/" + entry_name;
        }

        const std::size_t entry_size = tar_octal_size(block + 124, 12);
        const std::size_t data_offset = offset + 512;
        if (data_offset + entry_size > tar.size()) throw std::runtime_error("tar: truncated entry");
        if ((typeflag == '0' || typeflag == '\0') && entry_name == "APKINDEX") {
            members.emplace_back(tar, data_offset, entry_size);
        }
        offset = data_offset + ((entry_size + 511) / 512) * 512;
    }
    return members;
}

// ── APKINDEX record accumulation ─────────────────────────────────────────────────

/** Kotlin's shared mutable maps inside parseFullApkIndex (one pass per member). */
struct IndexAccumulator {
    std::map<std::string, FullPkgEntry> packages;
    std::map<std::string, std::string> so_to_pkg;               // raw so: flush entries
    std::map<std::string, std::vector<std::string>> provides;   // name → alias words ('=' cut)
    std::map<std::string, int> priorities;                      // k: field per package
};

void parse_records(std::string_view apkindex_text, IndexAccumulator& acc) {
    std::map<char, std::string> fields;

    const auto flush_record = [&] {
        const auto field = [&](char key) -> std::string {
            const auto it = fields.find(key);
            return it == fields.end() ? "" : it->second;
        };
        const std::string name = field('P');
        const std::string version = field('V');
        const std::string provided = field('p');
        if (name.rfind("so:", 0) == 0 && !provided.empty()) {
            acc.so_to_pkg[name] = provided;
        } else if (!name.empty() && !version.empty()) {
            acc.packages[name] = FullPkgEntry{name, version, apk_index_words(field('D'))};
            std::vector<std::string> aliases;
            for (std::string alias : apk_index_words(provided)) {
                const auto pin = alias.find('=');
                if (pin != std::string::npos) alias.resize(pin);  // substringBefore('=')
                aliases.push_back(std::move(alias));
            }
            acc.provides[name] = std::move(aliases);
            acc.priorities[name] = kotlin_int(field('k')).value_or(0);
        }
        fields.clear();
    };

    std::string text(apkindex_text);
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string raw = text.substr(start, end - start);
        start = end + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string line = trim_copy(raw);
        if (line.empty()) {
            flush_record();
        } else if (line.size() >= 2 && line[1] == ':') {
            fields[line[0]] = trim_copy(line.substr(2));
        }
        if (end == text.size()) break;
    }
    flush_record();
}

/** Kotlin's post-loop resolution: preferred wins, then k: priority, then name order. */
void resolve_provides(IndexAccumulator& acc, const std::set<std::string>& preferred_packages) {
    for (const auto& [name, aliases] : acc.provides) {
        for (const std::string& alias : aliases) {
            const auto previous_it = acc.so_to_pkg.find(alias);
            const std::string* previous =
                previous_it == acc.so_to_pkg.end() ? nullptr : &previous_it->second;
            const bool preferred = preferred_packages.count(name) != 0;
            const bool previous_preferred =
                previous != nullptr && preferred_packages.count(*previous) != 0;
            const int priority = acc.priorities.count(name) ? acc.priorities.at(name) : 0;
            const int previous_priority =
                (previous != nullptr && acc.priorities.count(*previous))
                    ? acc.priorities.at(*previous)
                    : 0;
            bool replace = false;
            if (previous == nullptr) {
                replace = true;
            } else if (preferred != previous_preferred) {
                replace = preferred;
            } else if (priority != previous_priority) {
                replace = priority > previous_priority;
            } else {
                replace = name < *previous;
            }
            if (replace) acc.so_to_pkg[alias] = name;
        }
    }
    acc.provides.clear();
    acc.priorities.clear();
}

}  // namespace

int compare_alpine_versions(const std::string& a, const std::string& b) {
    const auto [base_a, rev_a] = split_version(a);
    const auto [base_b, rev_b] = split_version(b);
    const std::vector<std::string> tokens_a = tokenise(base_a);
    const std::vector<std::string> tokens_b = tokenise(base_b);
    const std::size_t count = std::max(tokens_a.size(), tokens_b.size());
    for (std::size_t idx = 0; idx < count; ++idx) {
        const std::string ta = idx < tokens_a.size() ? tokens_a[idx] : "";
        const std::string tb = idx < tokens_b.size() ? tokens_b[idx] : "";
        if (ta == "_" && tb == "_") continue;
        if (ta == "_") return -1;  // '_' sorts below everything except another '_'
        if (tb == "_") return 1;
        if (ta == tb) continue;
        const int cmp = compare_token(ta, tb);
        if (cmp != 0) return cmp;
    }
    if (rev_a == rev_b) return 0;
    return rev_a < rev_b ? -1 : 1;
}

std::vector<std::string> apk_index_words(const std::string& value) {
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        const std::size_t start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        if (i > start) words.push_back(value.substr(start, i - start));
    }
    return words;
}

ApkIndex parse_full_apk_index(std::string_view apkindex_text,
                              const std::set<std::string>& preferred_packages) {
    IndexAccumulator acc;
    parse_records(apkindex_text, acc);
    resolve_provides(acc, preferred_packages);
    return ApkIndex{std::move(acc.packages), std::move(acc.so_to_pkg)};
}

ApkIndex parse_full_apk_index_gzip(const std::filesystem::path& index_file,
                                   const std::set<std::string>& preferred_packages) {
    std::ifstream input(index_file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + index_file.string());
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    const std::string tar = gunzip(bytes);

    // Kotlin shares the result/provides/priorities maps across members, flushes record fields
    // at every member boundary, and runs the provides resolution once at the very end.
    IndexAccumulator acc;
    for (const std::string& member : apkindex_members(tar)) {
        parse_records(member, acc);
    }
    resolve_provides(acc, preferred_packages);
    return ApkIndex{std::move(acc.packages), std::move(acc.so_to_pkg)};
}

std::vector<std::string> collect_alpine_package_changes(
    const std::vector<std::string>& requested,
    const std::map<std::string, FullPkgEntry>& packages,
    const std::map<std::string, std::string>& providers,
    const std::map<std::string, std::string>& installed) {
    std::vector<std::string> changes;
    std::vector<std::string> visited;

    std::function<void(const std::string&)> collect = [&](const std::string& name) {
        if (std::find(visited.begin(), visited.end(), name) != visited.end()) return;
        visited.push_back(name);
        const auto entry_it = packages.find(name);
        if (entry_it == packages.end()) return;
        const auto installed_it = installed.find(name);
        if (installed_it == installed.end() ||
            compare_alpine_versions(entry_it->second.version, installed_it->second) > 0) {
            if (std::find(changes.begin(), changes.end(), name) == changes.end()) {
                changes.push_back(name);
            }
        }
        for (const std::string& dependency : entry_it->second.deps) {
            const auto cut = dependency.find_first_of("=><~");
            const std::string dependency_name =
                cut == std::string::npos ? dependency : dependency.substr(0, cut);
            if (packages.count(dependency_name)) {
                collect(dependency_name);
            } else {
                const auto provider_it = providers.find(dependency_name);
                if (provider_it != providers.end()) collect(provider_it->second);
            }
        }
    };

    for (const std::string& name : requested) collect(name);
    return changes;
}

}  // namespace agora::core::sandbox