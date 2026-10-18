# glob-matcher golden stand

Parity pin for the native port of the portable path-glob matcher (`PortableGlobMatcher.kt`).
Every scenario under `scenarios/` runs through the **real app source compiled verbatim** and the
canonical trace is frozen under `golden/`; the C++ port (`agora::core::glob`, test
`core/tests/portable_glob_parity_test.cpp`) must reproduce every trace byte for byte.

`PortableGlobMatcher.kt` has no imports and no Android types, so it compiles standalone and the
driver calls `matches` directly — **no transcription anywhere in this stand**.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 73 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `GlobMatcherTraceDump.kt` | The stand driver (one `match\|0/1` line) |
| `run_golden.sh` | Compiles the Kotlin source and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## Scenario format

`{name, mode: "match", pattern, path}` →

```
name|<scenario name>
mode|match
match|0|1
```

## Pinned semantics (the port mirrors the produced regex, not the glob, on purpose)

The Kotlin code compiles the glob into `^…$` and asks a Java regex, so the pinned behaviour is
the **regex's**, quirks included:

* `\` normalizes to `/` in both pattern and path; `*` becomes `[^/]*` (a class, so it matches
  newlines), `**` becomes `.*` (or `(?:.*/)?` when a separator follows), `?` becomes `[^/]` and
  matches exactly one **code point** (an astral character counts as one).
* `.` never matches a line terminator: `a**b` does not match `a\nb`, while `a*b` does.
* Character classes keep the Java class grammar, not a naive set:
  * `[a-z]` is a range; a leading or trailing `-` is literal, and `[-a]`/`[--a]` are literals too;
  * `&&` intersects (`[a&&b]` matches nothing, `[a&&]`/`[&&a]` ignore the empty operand);
  * an **empty** class (`[]`, `[!]`) and any class whose body contains a nested `[` are
    `PatternSyntaxException`s, and `runCatching` turns them into `false`;
  * `[!…]` negates, `[^]` at index 0 is escaped by `escapeCharacterClass` and stays literal.
* Matching is a full-region match, so a trailing newline in the path never satisfies `$`.

The port implements these rules directly (a token program plus backtracking) rather than
embedding a regex engine; the corpus pins each rule above.

## Scope

Only the matcher. The callers that decide *which* paths to match (attachment validation,
repository scans) stay in Kotlin for now — see `core/README.md`.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run.