# message-source golden stand

Parity pin for `model/MessageSource.kt` — the structured record carried by messages the app sends
on the user's behalf (task prompts, loop prompts, ask_user answers), including its canonical
kotlinx-serialization wire form used by backup import. `MessageSource.kt` compiles **verbatim from
the app tree**; the canonical trace is frozen under `golden/` and the shipped port must reproduce
it byte for byte (`core/tests/message_source_parity_test.cpp`).

## What the stand compiles, and why

| File | Role |
| --- | --- |
| `model/MessageSource.kt` | data class, `Kind`/`AskUserItem`, encode/decode/sanitize |
| `extracted/Participant.kt` | `Participant` only, lifted declaration-for-declaration from `ChatMessage.kt` by `agora_extract participant` |
| `stubs/androidx/compose/runtime/Immutable.kt` | compile-only Compose stability annotation (same platform stub as the other stands) |

`ChatMessage.kt` drags the Room entity graph, Compose and kotlinx.serialization along, but
`MessageSource` only needs the three-value `Participant` enum. The extraction script regenerates
it before every compile; the enum is never retyped.

## What the port is

`agora::core::message_source` (`core/src/message_source.cpp`) reuses the core JSON layer, which
already mirrors the kotlinx lexer and the kotlinx compact writer (`escape_to`, lowercase
`\u00xx`), so the canonical stored form matches byte for byte. The codec configuration is the
app's own: `ignoreUnknownKeys = true`, `encodeDefaults = false`.

## What is pinned

* **decode degradation** — null/empty/blank stored values, invalid JSON, trailing content,
  concatenated values, a BOM, single quotes and unclosed objects all decode to null. Unknown keys
  are ignored at every level, including nested objects inside `askUser` items.
* **type rules** — `kind` must be a string and one of the serial names `task`/`loop`/`ask_user`
  (the Kotlin enum name `TASK` is not accepted); null/number/missing `kind` fail; `askUser` must
  be an array of objects; `question` is required and must be a string (null/number fail);
  `answer` may be absent, explicitly null or a string, but not a number/boolean.
* **constructor precondition** — `task`/`loop` with a non-empty `askUser` array is unreadable
  (the Kotlin `init` require throws during decoding), while an empty array is valid and is
  omitted from the canonical form.
* **canonical encode** — compact field order (`kind`, then `askUser`), defaults omitted
  (`askUser: []` and `answer: null` disappear), questions always present (even empty), unknown
  keys dropped, whitespace canonicalized, escaping pinned for quotes/backslash/newline/tab,
  control bytes (`\u0001 \u0007 \u000b \u001f`, lowercase hex) and raw UTF-8 (accented text,
  emoji, CJK are not escaped).
* **sanitizeImported** — only `USER` rows keep a source; `MODEL`/`ERROR` and blank/unreadable
  values become null. The kept value is re-encoded, so unknown keys and empty arrays are
  canonicalized away.
* **ask_userReadableText** — one `question\nanswer` block per item, blank line between items,
  `(No answer)` for an absent answer, empty answer stays an empty line, raw newlines/emoji/pipes
  pass through.
* **automation** — `"task"`/`"loop"` map to the shared `TASK`/`LOOP` values; other, empty, null
  and uppercase kinds map to null. `NO_ANSWER` and both shared values are pinned directly.

## Documented boundaries

* Duplicate JSON keys are not pinned: kotlinx does not document this codec's behaviour and no app
  encoder emits duplicates; the port reads the last occurrence (streaming assignment order).
* The Kotlin `askUser(items)` factory requires a non-empty list; every app call site builds it
  from at least one asked question, and the scenarios never call it empty.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (86 cases); rerun
`./run_golden.sh --freeze` afterwards. The stand needs the kotlinx-serialization compiler plugin
(found next to `kotlinc` by default).
