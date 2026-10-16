#!/usr/bin/env python3
"""Generates core/tools/golden/corpus.b64, the parity corpus.

One case per line, base64 of the UTF-8 text. The corpus mixes real text from the repository with
structured fuzz over the classes the o200k pre-tokenizer cares about (letters, marks, digits,
whitespace, contractions, punctuation) so that a segmentation disagreement shows up locally, not
on a phone. Deterministic: same seed, same corpus.

Usage: python3 generate_corpus.py <repo root> <output corpus.b64>
"""

import base64
import random
import sys
from pathlib import Path

SEED = 20261016

ASCII_LOWER = "abcdefghijklmnopqrstuvwxyz"
ASCII_UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
DIGITS = "0123456789"
PUNCT = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
LETTERS = "aAſф中日本語한글مرحباåéßẞǅˆ\u0301\u0308"
EMOJI = "🚀🔥👍🏽🇺🇳😀🧑‍💻"
WHITESPACE = " \t\n\r\x0b\x0c\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000"
CONTRACTIONS = ["'s", "'t", "'re", "'ve", "'m", "'ll", "'d", "'S", "'T", "'RE", "'Ve", "'M", "'LL", "'D", "'ſ"]

REF_CASES = [
    "The quick brown fox jumps over the lazy dog.",
    "Hello, world!",
    "今天天气很好，我想出门跑步。",
    "Agora 是一个 Android 上的本地 AI 客户端。",
    "🚀 ship it 🔥🔥",
    "fun main() {\n    println(\"hi\")\n}\n",
    '{"role": "user", "content": [{"type": "text", "text": "ok"}]}',
    "a    b\n\n\nc\t\td   ",
    "1234567890 3.14159 0042",
    "!!!???...---***///\\",
    "<|endoftext|> and <|im_start|>",
    "hahahahahahahahahahahahaha",
    "https://example.com/path?query=1&x=2#frag",
    "a",
    "中",
    "café naïve résumé",
    "привет мир как дела",
    "مرحبا بالعالم",
    "supercalifragilisticexpialidocious",
    "\n",
    " ",
    "CamelCaseWithUPPERInside",
    "# Heading\n\n- item *one*\n- `code`\n",
    "don't can't won't it's he's we're they've I'll you'd",
    "x\u0085y\u2028z\u2029 w\x0b\x0cv",
    "a\u2028\u2029b",
    "DON'T CAN'T HE'S WE'RE THEY'VE I'LL YOU'D",
    "groſſ'ſt'ſre",
]


def random_run(random, alphabet, max_len=12):
    return "".join(random.choice(alphabet) for _ in range(random.randint(1, max_len)))


def structured_case(random):
    parts = []
    for _ in range(random.randint(1, 8)):
        kind = random.random()
        if kind < 0.30:
            parts.append(random_run(random, ASCII_LOWER + ASCII_UPPER, 14))
        elif kind < 0.42:
            parts.append(random_run(random, DIGITS, 8))
        elif kind < 0.54:
            parts.append(random_run(random, PUNCT, 10))
        elif kind < 0.64:
            parts.append(random_run(random, WHITESPACE, 6))
        elif kind < 0.74:
            parts.append(random_run(random, LETTERS, 8))
        elif kind < 0.80:
            parts.append(random_run(random, EMOJI, 3))
        elif kind < 0.92:
            parts.append(random.choice(CONTRACTIONS))
            parts.append(random_run(random, ASCII_LOWER + ASCII_UPPER, 6))
        else:
            parts.append(random.choice("'’\"`-–—/\\|<>[]{}()"))
    return "".join(parts)


def random_codepoints_case(random):
    pool = (
        [chr(c) for c in range(0x20, 0x7F)] * 6
        + [chr(c) for c in range(0xA0, 0x100)]
        + [chr(c) for c in range(0x300, 0x360)]
        + [chr(c) for c in range(0x400, 0x460)]
        + ["中", "日", "本", "語", "한", "글", "مر", "حب", "ا", "ب", "ت"]
        + list(WHITESPACE)
        + list(EMOJI)
        + [chr(c) for c in (0x1C5, 0x1C6, 0x2C6, 0x17F, 0x2026, 0x2014, 0x2019, 0x00AB, 0x00BB)]
    )
    return "".join(random.choice(pool) for _ in range(random.randint(1, 80)))


def edge_cases():
    cases = []
    cases.append("a" * 600)
    cases.append("a" * 513)
    cases.append("a" * 512)
    cases.append("a" * 511)
    cases.append(" " * 300)
    cases.append("\n" * 50)
    cases.append("中" * 200)
    cases.append(("中" * 100 + "a" * 100) * 3)
    cases.append("9" * 600)
    cases.append(("ab" * 300))
    cases.append((" \n" * 100))
    cases.append(("\r\n" * 60))
    cases.append(("a" * 100 + "\n") * 5)
    cases.append("don't " * 120)
    cases.append("🚀" * 150)
    cases.append(("e\u0301" * 100))  # e + combining acute
    cases.append(("#" * 700))
    cases.append(("a b " * 150))
    cases.append("ſ" * 100)
    cases.append("ǅǅǅǅǅǅ")  # Lt
    cases.append("\u0301\u0301\u0301abc")
    cases.append("a\u0301b\u0308c")
    cases.append(" \u0301x")
    cases.append("x\u0301")
    cases.append("1970-01-01T00:00:00Z")
    cases.append("3.14159e-10 + 2E+8 = -4.2")
    cases.append("狩猎采集者")
    cases.append("服务器端渲染")
    cases.append("👨‍👩‍👧‍👦 family")
    return cases


def repo_text_cases(repo_root, random, limit=320):
    files = []
    for name in ("README.md", "README_RU.md", "LICENSE", "gradle.properties"):
        path = repo_root / name
        if path.is_file():
            files.append(path)
    docs = repo_root / "docs"
    if docs.is_dir():
        files.extend(sorted(docs.rglob("*.md"))[:40])
    src = repo_root / "app" / "src" / "main" / "java"
    if src.is_dir():
        files.extend(sorted(src.rglob("*.kt"))[::37][:40])
    cases = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if not text:
            continue
        for _ in range(max(1, limit // max(1, len(files)))):
            start = random.randrange(0, max(1, len(text) - 1))
            length = random.randint(1, 220)
            cases.append(text[start : start + length])
    return cases


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        raise SystemExit(2)
    repo_root = Path(sys.argv[1])
    output = Path(sys.argv[2])
    random.seed(SEED)

    cases = list(REF_CASES)
    cases.extend(edge_cases())
    cases.extend(repo_text_cases(repo_root, random))
    for _ in range(1400):
        cases.append(structured_case(random))
    for _ in range(800):
        cases.append(random_codepoints_case(random))

    seen = set()
    unique = []
    for case in cases:
        if not case:
            continue
        if case in seen:
            continue
        seen.add(case)
        unique.append(case)

    with output.open("w", encoding="ascii") as handle:
        for case in unique:
            handle.write(base64.b64encode(case.encode("utf-8")).decode("ascii"))
            handle.write("\n")
    print(f"{len(unique)} cases -> {output}")


if __name__ == "__main__":
    main()
