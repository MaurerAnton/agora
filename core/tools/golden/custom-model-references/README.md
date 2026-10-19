# custom-model-references golden stand

Parity pin for the native port of `data/CustomModelReferences.kt`. Every scenario under
`scenarios/` runs through the **real Kotlin file compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port
(`agora::core::custom_model_references`, test
`core/tests/custom_model_references_parity_test.cpp`) must reproduce every trace byte for byte.

`CustomModelReferences.kt` has **no imports at all** — three extension functions over containers —
so this stand needs no stubs and no source extraction. The whole file compiles as written.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 42 frozen cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `CustomModelReferencesTraceDump.kt` | The stand driver |
| `run_golden.sh` | Compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

The functions look like string substitution; what makes them worth pinning is that Kotlin's
containers are **ordered**, and the persisted preference is read back in iteration order.

**`Set<String>.replaceModelReference`** (`buildSet`, i.e. a `LinkedHashSet`):

* the replacement takes the **position of the id it replaces** — `["a", "c"]` becomes `["b", "c"]`,
  not `["c", "b"]`;
* if the replacement already exists, the result keeps **one** entry, and `buildSet`'s duplicate
  insertion keeps the first position: both `["a", "c"]` and `["c", "a"]` become `["c"]`;
* a `null` replacement removes the id; an absent id changes nothing;
* an empty id is a real id: replacing `""` works like any other value.

**`String?.replaceModelReference`** is the scalar form: only an exact match is rewritten, `null`
stays `null` regardless of the other arguments, and a match with a `null` replacement becomes
`null`.

**`Map<String, String>.replaceCustomModelAlias`** is `toMutableMap().apply { remove(old); if (new
!= null && alias.isNotBlank()) this[new] = alias.trim() }`, and the order of those two operations
is observable:

* `old == new` **moves the entry to the end** — `{a:1, b:2}` renamed `a -> a` with alias `3`
  becomes `{b:2, a:3}`;
* a target that already exists keeps **its** position and takes the new value —
  `{a:1, b:2}` renamed `a -> b` with `3` becomes `{b:3}`, regardless of which came first;
* an **absent** old id still receives the alias: `{a:1}` renamed `x -> b` becomes `{a:1, b:y}`;
* a `null` new id removes the old entry and adds nothing;
* a blank alias removes the old entry and adds nothing — including when `old == new`.

`isNotBlank()` is Kotlin's whitespace predicate, not ASCII:

* a **no-break space** (U+00A0) is whitespace, so an NBSP-only alias is blank and the entry is
  removed; an NBSP-padded alias is trimmed down to its content;
* a **zero-width space** (U+200B) is *not* whitespace: a ZWSP-only alias is stored as content, and
  ZWSP padding survives `trim()`. A port that trims "everything space-like" gets this wrong.

Interior whitespace is never collapsed: `"  x  y  "` becomes `"x  y"`.

## Scenario format (`scenarios/*.json`)

```json
{"name": "map_old_equals_new_moves_to_end", "op": "map",
 "oldModelId": "a", "newModelId": "a", "alias": "3", "entries": [["a", "1"], ["b", "2"]]}
```

`op` is one of:

* `set` — `oldModelId`, `newModelId` (nullable), `items` (array of strings);
* `string` — `value` (nullable), `oldModelId`, `newModelId` (nullable);
* `map` — `oldModelId`, `newModelId` (nullable), `alias`, `entries` (array of `[key, value]`).

A `null` field and an absent field are equivalent; `""` is a real value. The input set/map is built
with `linkedSetOf` / `linkedMapOf`, so the scenario's array order is the iteration order the app
sees.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|<operation>
args|<op-specific arguments joined by |>
out|set|<elements joined by |>
out|string?|<escaped>|-
out|map|<key=>value pairs joined by |>
```

Escaping: `\` `|` `;` `=` `\n` `\r` `\t` → `\\` `\p` `\s` `\e` `\n` `\r` `\t`. `=` is escaped so
the map's `key=>value` separator cannot be confused with an id that contains an equals sign.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
