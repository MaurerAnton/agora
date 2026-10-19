#!/usr/bin/env python3
"""Generates the cjk-text scenarios. Deterministic; rerun after editing.

`containsCjk` is a range test over UTF-16 chars: Extension A (U+3400..U+4DBF), the unified block
(U+4E00..U+9FFF) and the compatibility block (U+F900..U+FAFF). Hiragana, katakana, hangul and CJK
punctuation are deliberately excluded, and supplementary-plane Han characters are ignored because
Kotlin iterates UTF-16 units (a surrogate half is never inside a BMP range).
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, text):
    ALL.append({"name": name, "op": "containsCjk", "text": text})


case("empty", "")
case("ascii", "hello world")
case("single_han", "中")
case("han_inside_ascii", "abc中文def")
case("hiragana_is_excluded", "あ")
case("katakana_is_excluded", "ア")
case("hangul_is_excluded", "한")
case("cjk_punctuation_is_excluded", "。、「」")
case("ideographic_zero_is_excluded", "〇")          # U+3007
case("kanji_iteration_mark_is_excluded", "々")      # U+3005
case("fullwidth_latin_is_excluded", "ＡＢＣ")       # U+FF21..
case("emoji_is_excluded", "😀")

# Extension A: U+3400..U+4DBF.
case("ext_a_low_boundary", "\u3400")
case("ext_a_high_boundary", "\u4dbf")
case("below_ext_a", "\u33ff")
case("above_ext_a", "\u4dc0")

# Unified ideographs: U+4E00..U+9FFF.
case("unified_low_boundary", "\u4e00")
case("unified_high_boundary", "\u9fff")
case("above_unified", "\ua000")

# Compatibility ideographs: U+F900..U+FAFF.
case("compat_low_boundary", "\uf900")
case("compat_high_boundary", "\ufaff")
case("below_compat", "\uf8ff")
case("above_compat", "\ufb00")

# Supplementary planes: Kotlin sees a surrogate pair, so both halves miss every BMP range.
case("supplementary_ext_b_is_ignored", "\U00020000")
case("supplementary_compat_is_ignored", "\U0002f800")
case("supplementary_not_adjacent_to_range", "\U0002a6d6")

# Whitespace and control characters are content, and never Han.
case("whitespace_only", " \t\n\u00a0")
case("newline_around_han", "\n中\n")
case("zero_width_space_is_not_han", "\u200b")

os.makedirs(SCENARIOS_DIR, exist_ok=True)
for stale in os.listdir(SCENARIOS_DIR):
    if stale.endswith(".json"):
        os.remove(os.path.join(SCENARIOS_DIR, stale))
for entry in ALL:
    path = os.path.join(SCENARIOS_DIR, entry["name"] + ".json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(entry, handle, ensure_ascii=False)
        handle.write("\n")
print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")
