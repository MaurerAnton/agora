#include "agora/core/portable_glob.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::glob {
namespace {

// ---------------------------------------------------------------------------------------------
// The token program: the shape of the regex `globBodyToRegex` builds, matched with the same
// backtracking a Java regex performs. `.` never crosses a line terminator, character classes do.
// ---------------------------------------------------------------------------------------------

enum class TokenKind {
    Literal,             // Regex.escape of one Char
    AnyNotSlash,         // [^/]
    AnyNotSlashStar,     // [^/]*
    DotStar,             // .*
    OptionalDotStarSlash,  // (?:.*/)?
    Class,               // [...] or [^...]
};

struct Token {
    TokenKind kind = TokenKind::Literal;
    char32_t literal = 0;
    bool negated = false;
    // Class: an intersection (`&&`) of class parts, each a list of ranges (a lone member is a
    // range of one). Mirrors the Java class grammar the produced regex uses.
    std::vector<std::vector<std::pair<char32_t, char32_t>>> parts;
};

bool is_line_terminator(char32_t cp) {
    return cp == 0x0A || cp == 0x0D || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
}

std::string normalize(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) out.push_back(c == '\\' ? '/' : c);
    return out;
}

/** The UTF-16 code units of the text — the iteration domain of Kotlin's Char indexing. */
std::vector<char32_t> utf16_units(std::string_view text) {
    std::vector<char32_t> units;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F;
            extra = 2;
        } else {
            cp = lead & 0x07;
            extra = 3;
        }
        ++i;
        for (std::size_t k = 0; k < extra && i < text.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        }
        if (cp > 0xFFFF) {
            const char32_t value = cp - 0x10000;
            units.push_back(static_cast<char32_t>(0xD800 + (value >> 10)));
            units.push_back(static_cast<char32_t>(0xDC00 + (value & 0x3FF)));
        } else {
            units.push_back(cp);
        }
    }
    return units;
}

/** The UTF-16 code units of the text as code points (classes and `.` match whole code points). */
std::vector<char32_t> code_points(std::string_view text) {
    std::vector<char32_t> points;
    const std::vector<char32_t> units = utf16_units(text);
    std::size_t i = 0;
    while (i < units.size()) {
        const char32_t unit = units[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            points.push_back(0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            i += 2;
            continue;
        }
        points.push_back(unit);
        ++i;
    }
    return points;
}

/** Builds the token program; returns false for an invalid pattern (an empty character class). */
bool build_program(const std::vector<char32_t>& glob, std::vector<Token>& program) {
    std::size_t index = 0;
    while (index < glob.size()) {
        const char32_t unit = glob[index];
        if (unit == U'*') {
            if (index + 1 < glob.size() && glob[index + 1] == U'*') {
                index += 2;
                if (index < glob.size() && glob[index] == U'/') {
                    program.push_back(Token{TokenKind::OptionalDotStarSlash, 0, false, {}});
                    ++index;
                } else {
                    program.push_back(Token{TokenKind::DotStar, 0, false, {}});
                }
                continue;
            }
            program.push_back(Token{TokenKind::AnyNotSlashStar, 0, false, {}});
        } else if (unit == U'?') {
            program.push_back(Token{TokenKind::AnyNotSlash, 0, false, {}});
        } else if (unit == U'[') {
            std::size_t end = glob.size();
            for (std::size_t i = index + 1; i < glob.size(); ++i) {
                if (glob[i] == U']') {
                    end = i;
                    break;
                }
            }
            if (end == glob.size()) {
                program.push_back(Token{TokenKind::Literal, unit, false, {}});  // unclosed → literal
            } else {
                Token token;
                token.kind = TokenKind::Class;
                std::size_t start = index + 1;
                if (start < end && glob[start] == U'!') {
                    token.negated = true;
                    ++start;
                }
                // A nested `[` can never close inside this dialect (the class body ends at the
                // first `]`), so it is the JVM's "unclosed character class" failure.
                bool has_nested = false;
                for (std::size_t i = start; i < end && !has_nested; ++i) {
                    has_nested = glob[i] == U'[';
                }
                if (has_nested) return false;

                // Split on `&&` (empty operands are ignored, like the JVM), then parse ranges.
                std::vector<char32_t> part;
                std::vector<std::vector<char32_t>> segments;
                for (std::size_t i = start; i < end; ++i) {
                    if (glob[i] == U'&' && i + 1 < end && glob[i + 1] == U'&') {
                        segments.push_back(std::move(part));
                        part.clear();
                        ++i;
                        continue;
                    }
                    part.push_back(glob[i]);
                }
                segments.push_back(std::move(part));
                for (const std::vector<char32_t>& members : segments) {
                    if (members.empty()) continue;  // `[a&&]` / `[&&a]`
                    std::vector<std::pair<char32_t, char32_t>> ranges;
                    for (std::size_t i = 0; i < members.size();) {
                        // A range needs a non-'-' start, a '-' and a following character.
                        if (i + 2 < members.size() && members[i] != U'-' && members[i + 1] == U'-') {
                            ranges.emplace_back(members[i], members[i + 2]);
                            i += 3;
                        } else {
                            ranges.emplace_back(members[i], members[i]);
                            ++i;
                        }
                    }
                    token.parts.push_back(std::move(ranges));
                }
                if (token.parts.empty()) return false;  // `[]` / `[^]`: PatternSyntaxException
                program.push_back(std::move(token));
                index = end;
            }
        } else {
            program.push_back(Token{TokenKind::Literal, unit, false, {}});
        }
        ++index;
    }
    return true;
}

bool part_matches(const std::vector<std::pair<char32_t, char32_t>>& ranges, char32_t cp) {
    for (const auto& [low, high] : ranges) {
        if (cp >= low && cp <= high) return true;
    }
    return false;
}

bool class_matches(const Token& token, char32_t cp) {
    // Intersection of the parts, negated classes complement the whole intersection.
    bool member = true;
    for (const auto& part : token.parts) {
        if (!part_matches(part, cp)) {
            member = false;
            break;
        }
    }
    return token.negated ? !member : member;
}

bool match_from(const std::vector<Token>& program, std::size_t token_index,
                const std::vector<char32_t>& text, std::size_t text_index) {
    if (token_index == program.size()) return text_index == text.size();
    const Token& token = program[token_index];
    switch (token.kind) {
        case TokenKind::Literal:
            return text_index < text.size() && text[text_index] == token.literal &&
                   match_from(program, token_index + 1, text, text_index + 1);
        case TokenKind::AnyNotSlash:
            return text_index < text.size() && text[text_index] != U'/' &&
                   match_from(program, token_index + 1, text, text_index + 1);
        case TokenKind::Class:
            return text_index < text.size() && class_matches(token, text[text_index]) &&
                   match_from(program, token_index + 1, text, text_index + 1);
        case TokenKind::AnyNotSlashStar: {
            std::size_t cursor = text_index;
            while (true) {
                if (match_from(program, token_index + 1, text, cursor)) return true;
                if (cursor >= text.size() || text[cursor] == U'/') return false;
                ++cursor;
            }
        }
        case TokenKind::DotStar: {
            std::size_t cursor = text_index;
            while (true) {
                if (match_from(program, token_index + 1, text, cursor)) return true;
                if (cursor >= text.size() || is_line_terminator(text[cursor])) return false;
                ++cursor;
            }
        }
        case TokenKind::OptionalDotStarSlash: {
            // Greedy first (like the regex), then the empty alternative.
            std::size_t cursor = text_index;
            while (true) {
                if (cursor < text.size() && text[cursor] == U'/' &&
                    match_from(program, token_index + 1, text, cursor + 1)) {
                    return true;
                }
                if (cursor >= text.size() || is_line_terminator(text[cursor])) break;
                ++cursor;
            }
            return match_from(program, token_index + 1, text, text_index);
        }
    }
    return false;
}

}  // namespace

bool matches(std::string_view pattern, std::string_view path) {
    std::vector<Token> program;
    if (!build_program(utf16_units(normalize(pattern)), program)) return false;
    return match_from(program, 0, code_points(normalize(path)), 0);
}

}  // namespace agora::core::glob