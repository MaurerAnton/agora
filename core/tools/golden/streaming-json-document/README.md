# streaming-json-document golden stand

Parity pin for the prefix-aware JSON parser and the compact tool-card hint resolver:

* `ui/chat/message/StreamingJsonDocument.kt` — parser + document tree
* `ui/chat/message/StreamingToolArgumentHints.kt` — bounded hint extraction + subject normalizer

Every scenario under `scenarios/` runs through the **real Kotlin sources compiled verbatim from
the app tree** and the canonical trace is frozen under `golden/`; the shipped port must reproduce
every trace byte for byte (`core/tests/streaming_json_parity_test.cpp`).

## What the stand compiles, and why

| File | Role |
| --- | --- |
| `ui/chat/message/StreamingJsonDocument.kt` | `StreamingJson*` data classes and `StreamingJsonParser` (zero imports) |
| `ui/chat/message/StreamingToolArgumentHints.kt` | `StreamingToolArgumentHintResolver`, `normalizeToolSummarySubject` (zero imports) |
| `ui/chat/message/ToolPresentation.kt` | `ToolKind` only, lifted by script |

`ToolPresentation.kt` drags kotlinx.serialization and `ChatMessage` along, so the enum is not
compiled whole. `extract_tool_kind.py` copies the `ToolKind` declaration verbatim into
`extracted/ToolKind.kt` and `run_golden.sh` regenerates it before every compile — the enum is
never retyped. The UI that consumes a `StreamingJsonDocument` (`MessageItemJson.kt`, Compose
rendering) is out of scope; this stand pins the pure parser/hint logic beneath it.

## What the port is

`agora::core::streaming_json` (`core/src/streaming_json.cpp`). The Kotlin sources are pure
`String` logic, so the port keeps every cursor, `errorOffset`, length and slice in **UTF-16 code
units**, like the JVM: the document is parsed from a `std::u16string` and only converted to UTF-8
at the `describe`/`Hints` boundary. `errorOffset` is a UI cursor into the UTF-16 argument string,
and the `normalizeMax` truncation counts UTF-16 units too — the traces pin both with emoji and
accented input.

## What is pinned

* **document status** — empty/whitespace input is `INCOMPLETE` with a null root; truncation
  (`{"a":`, `[1,`, `"abc`, `12.`, `1e+`, `tru`) keeps the open leaf and stays `INCOMPLETE`; an
  impossible prefix (`{a:1}`, `{"a":1,}`, `[1,]`, `01`, `1.x`, control bytes, bad escapes) is
  `INVALID` with the exact `errorOffset`; a closed document is `COMPLETE`.
* **multiple roots** — values separated strictly by JSON whitespace accumulate in `roots`
  (`1 2`, `{} []`); concatenation without a boundary (`{}[]`, `{} x`) is `INVALID` at the
  offending offset; `root` is always `roots.firstOrNull()`.
* **open leaves** — partial objects/arrays keep every completed entry plus the unfinished one
  (`key`, `keyComplete`, nullable `value`), so the UI can render while arguments stream.
* **string escapes** — `\" \\ \/ \b \f \n \r \t` decode; `\uXXXX` assembles UTF-16 units
  (`\u0041`, `\u00e9`, an emoji surrogate pair); a truncated `\u12` (end of input) is
  `INCOMPLETE`, while `\u12g4` and `\x`-style bad escapes are `INVALID` at the escape character.
* **UTF-16 offsets** — `"🙂\q"` reports `errorOffset=4` and `"é\q"` reports `3`, i.e. an astral
  code point counts as two units. A byte-indexed port fails these two traces.
* **numbers** — JSON grammar only: leading zero ends the number and invalidates the tail,
  `1.x`/`1ex` fail after the separator, `12.`/`1e`/`1e+`/`-` are truncation, `-0.5e-10` is one
  scalar.
* **hints** — every `ToolKind` branch is driven: `names` selection for `MEMORY_READ`/`SKILL_READ`
  (single complete name vs. partial, empty vs. blank entries, incomplete array, count only when
  the array is complete and selected), the scalar fallbacks (`name`, `query`, `url`,
  `conversation_id`, `command`, `job_id`, `path`, `pattern`, `prompt`, `id_or_name → name →
  task_id`), `server`/`operation`/`new_name`, and the kinds that deliberately resolve to null.
* **bounded prefix** — only the first 4096 UTF-16 units are parsed. The
  `hints_prefix_boundary_*` pair places `"new_name":"dst"` just inside and just outside that
  boundary; the trace differs in exactly one field (`destination=dst` vs `null`).
* **subject normalization** — regex `\s+` covers ASCII `[ \t\n\x0B\f\r]` (so a vertical tab and a
  form feed collapse), Kotlin `trim()` removes NBSP and friends, blank/empty results are null,
  exactly 120 UTF-16 units pass through, 121 units become `take(119) + "…"`, and the same rule is
  pinned with an astral run to show unit counting (59 emoji + `xyz` truncates after `x`).

## Documented boundaries

* `\uXXXX` digits and number-continuation digits are ASCII in the port. Kotlin routes them
  through `Character.digit` / `Char.isDigit` (Unicode Nd), which only differs for exotic digit
  forms that no JSON encoder emits; the scenarios pin the ASCII grammar.
* `normalizeToolSummarySubject` requires `maxCharacters > 0` in Kotlin (`require` throws). Every
  app call site passes a positive width; the port treats a non-positive width as unreachable and
  the scenarios never pass one.
* An unpaired surrogate can only appear if the caller feeds one in; the JVM UTF-8 encoder writes
  `?` for it, and the port's `utf16::to_utf8` does the same. The frozen scenarios stay on valid
  text so the traces are always valid UTF-8.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically; rerun
`./run_golden.sh --freeze` afterwards.

## Trace format

```
name|<scenario>
op|parse|hints|normalize
args|...                 (parse: the raw source; hints: kind|rawArguments; normalize: value|max)
result|...               (Kotlin data-class toString() of the document / hints, or the normalized
                          subject with `null` for absent)
```

`|`, `;`, `\`, newline, CR and tab are escaped in the driver, so the result is one field per
line.
