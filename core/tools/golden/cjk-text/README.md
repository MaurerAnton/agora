# cjk-text golden stand

Parity pin for the native port of `model/CjkText.kt`. Every scenario under `scenarios/` runs
through the **real Kotlin file compiled verbatim from the app tree** and the canonical trace is
frozen under `golden/`; the C++ port (`agora::core::cjk_text`, test
`core/tests/cjk_text_parity_test.cpp`) must reproduce every trace byte for byte.

`CjkText.kt` has **no imports at all** — three private ranges, one predicate, one constant — so
this stand needs no stubs and no source extraction. The whole file compiles as written.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 29 frozen cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `CjkTextTraceDump.kt` | The stand driver |
| `run_golden.sh` | Compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

`containsCjk()` is a **UTF-16 `Char` range test**, and the exact set of ranges is the contract:

* **Extension A** `U+3400..U+4DBF`;
* **unified ideographs** `U+4E00..U+9FFF`;
* **compatibility ideographs** `U+F900..U+FAFF`.

Every edge is frozen from both sides: `U+33FF`/`U+3400`, `U+4DBF`/`U+4DC0`, `U+4E00`/`U+9FFF`/
`U+A000`, `U+F8FF`/`U+F900`/`U+FAFF`/`U+FB00`.

The exclusions are deliberate and pinned:

* **Hiragana, Katakana and Hangul do not trigger** — only Han ideographs do;
* **CJK punctuation** (`。`, `、`, `「`, `」`) and the **iteration mark** `々` (U+3005) are outside
  the ranges, and so is the ideographic zero `〇` (U+3007);
* **supplementary-plane Han characters do not trigger**: Kotlin iterates UTF-16 units, so a
  surrogate half can never fall inside a BMP range. The traces freeze `U+20000`, `U+2F800` and
  `U+2A6D6` as `false`. A port that decodes code points and then checks the same BMP ranges gets
  the same answer; a port that "helpfully" adds the supplementary blocks does not.

The **retry-guard prompt** is part of every trace, because it is the user-visible instruction
appended to the retry request (and is never persisted). Changing it is a trace change.

Whitespace, NBSP and zero-width space are ordinary non-Han content.

## Scenario format (`scenarios/*.json`)

```json
{"name": "above_unified", "op": "containsCjk", "text": "\ua000"}
```

`text` is a required string; the scenarios use raw UTF-8 so the file shows the actual characters.
`\uXXXX` escapes are supported by the parser and used for the boundary cases.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|containsCjk
constants|<retry-guard prompt>
args|<escaped input text>
out|bool|true|false
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`. Non-ASCII text is written as
raw UTF-8, not escaped.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
