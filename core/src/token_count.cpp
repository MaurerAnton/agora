#include "agora/core/token_count.hpp"

#include <array>
#include <climits>
#include <cstring>
#include <optional>

#include "agora/core/unicode.hpp"

namespace agora::core {
namespace {

using unicode::Decoded;
using unicode::decode_utf8;

constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxMergeBytes = 512;

bool is_newline(std::uint32_t cp) { return cp == '\r' || cp == '\n'; }
bool is_ws(std::uint32_t cp) { return (unicode::flags_of(cp) & unicode::kWhitespace) != 0; }
bool is_upper_run(std::uint32_t cp) { return (unicode::flags_of(cp) & unicode::kUpperRun) != 0; }
bool is_lower_run(std::uint32_t cp) { return (unicode::flags_of(cp) & unicode::kLowerRun) != 0; }
bool is_letter(std::uint32_t cp) { return (unicode::flags_of(cp) & unicode::kLetter) != 0; }
bool is_number(std::uint32_t cp) { return (unicode::flags_of(cp) & unicode::kNumber) != 0; }

/** [^\r\n\p{L}\p{N}] — the one optional character a word piece may start with. */
bool is_prefix(std::uint32_t cp) { return !is_newline(cp) && !is_letter(cp) && !is_number(cp); }

/** [^\s\p{L}\p{N}] — the body of a punctuation piece. */
bool is_middle(std::uint32_t cp) { return !is_ws(cp) && !is_letter(cp) && !is_number(cp); }

/**
 * The o200k pre-tokenizer, as a leftmost-first scan over UTF-8 bytes.
 *
 * The Kotlin original runs the pattern below through java.util.regex on the JVM and ICU on
 * Android; this scanner reproduces the same alternation order and greedy/backtracking choices
 * codepoint by codepoint, so both sides split text into identical pieces:
 *
 *   [^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]*[\p{Ll}\p{Lm}\p{Lo}\p{M}]+'s|'t|'re|'ve|'m|'ll|'d?
 *  |[^\r\n\p{L}\p{N}]?[\p{Lu}\p{Lt}\p{Lm}\p{Lo}\p{M}]+[\p{Ll}\p{Lm}\p{Lo}\p{M}]*'s|'t|...
 *  |\p{N}{1,3}
 *  | ?[^\s\p{L}\p{N}]+[\r\n/]*
 *  |[\s]*[\r\n]+
 *  |[\s]+(?![^\s])
 *  |[\s]+
 */
struct Scanner {
    std::string_view text;
    std::size_t n;

    Decoded at(std::size_t offset) const {
        return decode_utf8(text.data() + offset, n - offset);
    }

    std::size_t run_end(std::size_t from, bool (*kind)(std::uint32_t)) const {
        while (from < n) {
            const Decoded decoded = at(from);
            if (!kind(decoded.code_point)) break;
            from += decoded.length;
        }
        return from;
    }

    std::size_t ws_run_end(std::size_t from) const { return run_end(from, is_ws); }
    std::size_t upper_run_end(std::size_t from) const { return run_end(from, is_upper_run); }
    std::size_t lower_run_end(std::size_t from) const { return run_end(from, is_lower_run); }
    std::size_t middle_run_end(std::size_t from) const { return run_end(from, is_middle); }

    std::size_t tail_run_end(std::size_t from) const {
        while (from < n) {
            const Decoded decoded = at(from);
            const std::uint32_t cp = decoded.code_point;
            if (cp != '\r' && cp != '\n' && cp != '/') break;
            from += decoded.length;
        }
        return from;
    }

    /** The contraction suffixes, matched case-insensitively, including the long s (U+017F). */
    std::size_t contraction_end(std::size_t from) const {
        if (from >= n) return kNoMatch;
        const Decoded quote = at(from);
        if (quote.code_point != '\'') return kNoMatch;
        const std::size_t after_quote = from + quote.length;
        if (after_quote >= n) return kNoMatch;
        const Decoded letter = at(after_quote);
        switch (letter.code_point) {
            case 's': case 'S': case 0x17F:
            case 't': case 'T':
            case 'm': case 'M':
            case 'd': case 'D':
                return after_quote + letter.length;
            case 'r': case 'R':
            case 'v': case 'V': {
                const std::size_t after = after_quote + letter.length;
                if (after >= n) return kNoMatch;
                const Decoded next = at(after);
                if (next.code_point == 'e' || next.code_point == 'E') return after + next.length;
                return kNoMatch;
            }
            case 'l': case 'L': {
                const std::size_t after = after_quote + letter.length;
                if (after >= n) return kNoMatch;
                const Decoded next = at(after);
                if (next.code_point == 'l' || next.code_point == 'L') return after + next.length;
                return kNoMatch;
            }
            default:
                return kNoMatch;
        }
    }

    std::size_t take_contraction(std::size_t from) const {
        const std::size_t end = contraction_end(from);
        return end == kNoMatch ? from : end;
    }

    /** The first two alternatives share a shape: an upper run, then a lower run. */
    std::size_t first_alternative(std::size_t q) const {
        const std::size_t upper_end = upper_run_end(q);
        if (upper_end < n && is_lower_run(at(upper_end).code_point)) {
            return take_contraction(lower_run_end(upper_end));
        }
        // The upper run backtracks one code point at a time. The first position that is also
        // in the lower class starts a one-character lower run: everything after it in the
        // upper run is, by construction, not in the lower class.
        std::size_t at_offset = upper_end;
        while (at_offset > q) {
            at_offset = unicode::previous_offset(text.data(), at_offset);
            const Decoded decoded = at(at_offset);
            if (is_lower_run(decoded.code_point)) {
                return take_contraction(at_offset + decoded.length);
            }
        }
        return kNoMatch;
    }

    std::size_t second_alternative(std::size_t q) const {
        const Decoded first = at(q);
        if (!is_upper_run(first.code_point)) return kNoMatch;
        const std::size_t upper_end = upper_run_end(q);
        const std::size_t lower_end = lower_run_end(upper_end);
        return take_contraction(lower_end);
    }

    std::size_t alt1(std::size_t p) const {
        const Decoded first = at(p);
        if (is_prefix(first.code_point)) {
            const std::size_t taken = first_alternative(p + first.length);
            if (taken != kNoMatch) return taken;
        }
        return first_alternative(p);
    }

    std::size_t alt2(std::size_t p) const {
        const Decoded first = at(p);
        if (is_prefix(first.code_point)) {
            const std::size_t taken = second_alternative(p + first.length);
            if (taken != kNoMatch) return taken;
        }
        return second_alternative(p);
    }

    std::size_t alt3(std::size_t p) const {
        const Decoded first = at(p);
        if (!is_number(first.code_point)) return kNoMatch;
        std::size_t end = p + first.length;
        for (int count = 1; count < 3 && end < n; ++count) {
            const Decoded next = at(end);
            if (!is_number(next.code_point)) break;
            end += next.length;
        }
        return end;
    }

    std::size_t alt4(std::size_t p) const {
        const Decoded first = at(p);
        std::size_t middle_start;
        if (first.code_point == ' ') {
            const std::size_t after_space = p + first.length;
            if (after_space >= n) return kNoMatch;
            if (!is_middle(at(after_space).code_point)) return kNoMatch;
            middle_start = after_space;
        } else if (is_middle(first.code_point)) {
            middle_start = p;
        } else {
            return kNoMatch;
        }
        return tail_run_end(middle_run_end(middle_start));
    }

    std::size_t alt5(std::size_t p) const {
        if (!is_ws(at(p).code_point)) return kNoMatch;
        const std::size_t run_end_offset = ws_run_end(p);
        // The whitespace run backtracks to the last newline inside it; [\r\n]+ then consumes
        // exactly that newline, because everything after it in the run is not a newline.
        std::size_t at_offset = run_end_offset;
        while (at_offset > p) {
            at_offset = unicode::previous_offset(text.data(), at_offset);
            const Decoded decoded = at(at_offset);
            if (is_newline(decoded.code_point)) {
                std::size_t end = at_offset + decoded.length;
                while (end < n) {
                    const Decoded next = at(end);
                    if (!is_newline(next.code_point)) break;
                    end += next.length;
                }
                return end;
            }
        }
        return kNoMatch;
    }

    /**
     * [$WS]+(?![^$WS]): a whitespace run gives back its last character when a non-whitespace
     * follows, because the lookahead then sees that last whitespace. Only a run of one character
     * has nothing to give back, and that alternative fails.
     */
    std::size_t alt6(std::size_t p) const {
        if (!is_ws(at(p).code_point)) return kNoMatch;
        const std::size_t run_end = ws_run_end(p);
        if (run_end == n) return run_end;
        const std::size_t shrunk = unicode::previous_offset(text.data(), run_end);
        return shrunk > p ? shrunk : kNoMatch;
    }

    std::size_t alt7(std::size_t p) const {
        if (!is_ws(at(p).code_point)) return kNoMatch;
        return ws_run_end(p);
    }

    std::size_t match_at(std::size_t p) const {
        std::size_t end = alt1(p);
        if (end != kNoMatch) return end;
        end = alt2(p);
        if (end != kNoMatch) return end;
        end = alt3(p);
        if (end != kNoMatch) return end;
        end = alt4(p);
        if (end != kNoMatch) return end;
        end = alt5(p);
        if (end != kNoMatch) return end;
        end = alt6(p);
        if (end != kNoMatch) return end;
        return alt7(p);
    }
};

constexpr int kNoRank = INT_MAX;

int pair_rank(
    const std::uint8_t* piece,
    const std::array<std::uint32_t, kMaxMergeBytes + 1>& starts,
    std::size_t part_count,
    std::size_t part,
    const BpeVocabulary& vocabulary
) {
    if (part + 2 > part_count - 1) return kNoRank;
    const int rank = vocabulary.rank_of(piece, starts[part], starts[part + 2]);
    return rank == BpeVocabulary::kAbsent ? kNoRank : rank;
}

/**
 * Merges `piece[from, to)` by rank and returns how many parts survive.
 *
 * Parts are held as their start offsets plus the rank of merging each part with the next one.
 * The lowest rank merges first, which only changes the ranks of the merged part and the one
 * before it, so the loop repairs those two and rescans for the next minimum.
 */
int merge_span(const std::uint8_t* piece, std::size_t from, std::size_t to, const BpeVocabulary& vocabulary) {
    const std::size_t length = to - from;
    if (length <= 1) return static_cast<int>(length);
    if (vocabulary.rank_of(piece, from, to) != BpeVocabulary::kAbsent) return 1;

    std::array<std::uint32_t, kMaxMergeBytes + 1> starts{};
    std::array<int, kMaxMergeBytes + 1> ranks{};
    std::size_t part_count = length + 1;
    for (std::size_t index = 0; index < part_count; ++index) {
        starts[index] = static_cast<std::uint32_t>(from + index);
    }
    for (std::size_t part = 0; part < part_count; ++part) {
        ranks[part] = pair_rank(piece, starts, part_count, part, vocabulary);
    }

    while (true) {
        int lowest_rank = kNoRank;
        std::size_t lowest_part = part_count;
        for (std::size_t part = 0; part + 1 < part_count; ++part) {
            if (ranks[part] < lowest_rank) {
                lowest_rank = ranks[part];
                lowest_part = part;
            }
        }
        if (lowest_part == part_count) break;

        // Merging means dropping the boundary that separates this part from the next.
        const std::size_t tail = part_count - lowest_part - 2;
        std::memmove(starts.data() + lowest_part + 1, starts.data() + lowest_part + 2, tail * sizeof(std::uint32_t));
        std::memmove(ranks.data() + lowest_part + 1, ranks.data() + lowest_part + 2, tail * sizeof(int));
        part_count--;
        ranks[lowest_part] = pair_rank(piece, starts, part_count, lowest_part, vocabulary);
        if (lowest_part > 0) {
            ranks[lowest_part - 1] = pair_rank(piece, starts, part_count, lowest_part - 1, vocabulary);
        }
    }
    return static_cast<int>(part_count) - 1;
}

int merge_count(const std::uint8_t* piece, std::size_t length, const BpeVocabulary& vocabulary) {
    if (length == 0) return 0;
    // A merge loop is quadratic in the length of one piece, so an unusually long piece is
    // merged in chunks; chunking can only split a merge that would have happened, which
    // overcounts slightly — the safe direction for a context estimate.
    if (length <= kMaxMergeBytes) return merge_span(piece, 0, length, vocabulary);
    int total = 0;
    for (std::size_t from = 0; from < length; from += kMaxMergeBytes) {
        const std::size_t to = std::min(from + kMaxMergeBytes, length);
        total += merge_span(piece, from, to, vocabulary);
    }
    return total;
}

}  // namespace

void for_each_piece(std::string_view utf8, const std::function<void(std::string_view)>& visit) {
    if (utf8.empty()) return;
    const Scanner scanner{utf8, utf8.size()};
    std::size_t at_offset = 0;
    while (at_offset < utf8.size()) {
        std::size_t end = scanner.match_at(at_offset);
        if (end == kNoMatch || end == at_offset) {
            // Unreachable for valid text: every code point starts one of the alternatives.
            end = at_offset + scanner.at(at_offset).length;
        }
        visit(utf8.substr(at_offset, end - at_offset));
        at_offset = end;
    }
}

std::int64_t bpe_token_count(std::string_view utf8, const BpeVocabulary& vocabulary) {
    if (utf8.empty()) return 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(utf8.data());
    std::int64_t total = 0;
    for_each_piece(utf8, [&](std::string_view piece) {
        total += merge_count(bytes + (piece.data() - utf8.data()), piece.size(), vocabulary);
    });
    return total;
}

}  // namespace agora::core
