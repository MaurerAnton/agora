# shell-tools golden stand

Parity pin for the native port of the shell tool JSON helpers and the bounded file-read
projection. Every scenario under `scenarios/` runs through the **real app sources** and the
canonical trace is frozen under `golden/`; the C++ port (`agora::core::shell_tools`, test
`core/tests/shell_tools_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 37 frozen edge cases across three modes |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ShellToolsTraceDump.kt` | The stand driver (calls the real functions) |
| ``agora_extract shell-models`` | Verbatim extraction of `ShellFileReadResult` + `shellFileLineCount` |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## How the stand works

`ShellToolJson.kt` and `BoundedFileReadJson.kt` compile **verbatim from the app tree** — the
driver calls them directly, no transcription. Only the two declarations they need from the
transport-heavy `ShellClient.kt` (`ShellFileReadResult`, `shellFileLineCount`) are extracted
verbatim into the generated `build/ShellModels.kt` (visibility unchanged).

## Scenario format and modes

| mode | Fields | Trace |
| --- | --- | --- |
| `toolArgs` | `arguments`, `probeKey` | `arg\|<key>\|<element>` per entry, then `probe\|<content\|->` and `bool\|0/1` |
| `jsonError` | `type`, `message`, `server?`, `command?` | `json\|<escaped JSON>` |
| `fileRead` | `fileServer`, `filePath`, `fileContent`, `totalLines`, `totalBytes`, `offset`, `limit`, `fileTruncated`, `maxLength?` | `json\|<escaped JSON>`, or `error\|IllegalArgumentException` for `maxLength <= 0` |

## Pinned semantics

* `parseToolArgs` blank payload → `{}`; malformed/non-object JSON → `{}`; duplicate keys keep
  the LAST value at the FIRST position (LinkedHashMap).
* `arg` returns `JsonPrimitive.content` — so an explicit `null` yields the literal text
  `"null"`, while objects/arrays yield `""`; `boolArg` accepts only the `true`/`false`
  spellings (`"TRUE"`, `1`, null → false).
* `jsonError` key order is `type?`, `error`, `message`, `server?`, `command?`; a blank `type`
  is omitted while the optional strings are included whenever present.
* `boundedFileReadJson` measures everything in **UTF-16 code units** (Kotlin `String.length`):
  the full-vs-max comparison, the binary search bounds and every candidate check. The search
  walks unit indexes; when a midpoint would split a surrogate pair the slice backs off one
  unit, so the pinned emoji corpus could never produce a lone surrogate.
* A `maxLength` smaller than the empty-content envelope returns that over-limit envelope with
  `truncated=true` (the function's `best` seed), pinned at limits 5 and 1; `<= 0` throws.
* `lines` recomputes from the surviving slice (`0` when empty), and `returned_bytes` is the
  UTF-8 byte size of that slice — both change between the full and truncated forms, which the
  fixed `maxLength` corpus pins indirectly.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
the declarations move or change shape.
