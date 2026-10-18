#!/usr/bin/env python3
"""Generates the scenario corpus for the glob-matcher golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, pattern, path):
    ALL.append({"name": name, "mode": "match", "pattern": pattern, "path": path})


# ---- literals -----------------------------------------------------------------------------
case("literal_exact", "a/b.txt", "a/b.txt")
case("literal_mismatch", "a/b.txt", "a/c.txt")
case("literal_prefix_only", "a", "ab")
case("empty_both", "", "")
case("empty_pattern_path", "", "a")
case("pattern_empty_path", "a", "")
case("literal_metacharacters", "a.b+c(d)", "a.b+c(d)")
case("literal_dot_does_not_match_star", "a.b", "axb")

# ---- star (single, segment-bound) ------------------------------------------------------------
case("star_suffix", "*.txt", "a.txt")
case("star_suffix_empty_stem", "*.txt", ".txt")
case("star_crosses_separator", "a*b", "a/b")
case("star_middle", "a*c", "abc")
case("star_newline_is_in_class", "a*b", "a\nb")
case("star_repeated", "a*b*c", "abc")
case("star_alone", "*", "anything")
case("star_alone_empty", "*", "")

# ---- doublestar ------------------------------------------------------------------------------
case("globstar_leading_optional_dir", "**/foo", "foo")
case("globstar_leading_deep", "**/foo", "a/b/foo")
case("globstar_middle_collapse", "a/**/foo", "a/foo")
case("globstar_middle_deep", "a/**/foo", "a/x/y/foo")
case("globstar_whole", "**", "a/b/c")
case("globstar_empty", "**", "")
case("globstar_adjacent_stars", "a**b", "a/b")
case("globstar_adjacent_newline_blocked", "a**b", "a\nb")
case("globstar_trailing", "a/**", "a/")
case("globstar_trailing_deep", "a/**", "a/b/c")
case("globstar_prefix_globstar_suffix", "src/**/build", "src/main/build")
case("globstar_star_mix", "**/*.kt", "a/b/c.kt")
case("globstar_star_mix_no_dir", "**/*.kt", "c.kt")

# ---- question mark ----------------------------------------------------------------------------
case("question_one_char", "a?c", "abc")
case("question_requires_char", "a?c", "ac")
case("question_excludes_separator", "a?c", "a/c")
case("question_astral_is_one_code_point", "a?c", "a😀c")
case("question_matches_newline", "a?c", "a\nc")
case("question_alone", "?", "x")
case("question_alone_separator", "?", "/")

# ---- character classes ------------------------------------------------------------------------
case("class_simple", "[abc]", "b")
case("class_miss", "[abc]", "d")
case("class_negated", "[!abc]", "d")
case("class_negated_hit", "[!abc]", "a")
case("class_empty_is_invalid_pattern", "[]", "]")
case("class_bang_only_is_invalid_pattern", "[!]", "x")
case("class_literal_open_bracket", "[[]", "[")
case("class_literal_close_bracket", "[]a]", "]")
case("class_escaped_backslash", "[\\\\]", "\\")
case("class_caret_escaped_at_zero", "[^]", "^")
case("class_caret_inside", "[a^]", "^")
case("class_range", "[a-c]", "b")
case("class_range_miss", "[a-c]", "d")
case("class_with_separator_member", "[/a]", "/")
case("class_unclosed_is_literal", "[abc", "[abc")
case("class_in_middle", "a[0-9]z", "a5z")
case("class_after_star", "*[0-9]", "abc7")
case("class_bang_inside", "[a!b]", "!")
case("class_multibyte", "[éü]", "é")
case("class_trailing_dash", "[a-]", "-")
case("class_leading_dash", "[-a]", "-")
case("class_double_dash_literals", "[--a]", "b")
case("class_double_dash_dash", "[--a]", "-")
case("class_intersection", "[a&&b]", "a")
case("class_intersection_empty_operand", "[a&&]", "a")
case("class_intersection_leading", "[&&a]", "a")
case("class_nested_open_is_invalid", "[a[b]", "a")
case("class_negated_with_intersection", "[!a&&b]", "c")

# ---- normalization ------------------------------------------------------------------------------
case("normalize_pattern_backslash", "src\\main", "src/main")
case("normalize_path_backslash", "src/main", "src\\main")
case("normalize_both_backslashes", "a\\b\\**\\c", "a/b/x/c")
case("normalize_backslash_not_special", "\\*", "*")

# ---- backtracking -------------------------------------------------------------------------------
case("backtrack_multiple_stars", "a*a*a*a*b", "aa/a/a/b")
case("backtrack_no_match", "a*a*a*a*b", "aa/a/a/c")
case("backtrack_star_then_globstar", "*/**/x", "a/b/x")
case("backtrack_empty_prefix", "*a", "a")
case("deep_path", "**/node_modules/**/package.json", "a/node_modules/b/node_modules/package.json")


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()