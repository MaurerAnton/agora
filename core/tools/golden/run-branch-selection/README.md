# run-branch-selection golden stand

Parity pin for the native port of `data/local/RunBranchSelectionIntegrity.kt`. Every scenario under
`scenarios/` runs through the **real Kotlin file compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port
(`agora::core::run_branch_selection`, test
`core/tests/run_branch_selection_parity_test.cpp`) must reproduce every trace byte for byte.

## What the stand has to extract, and why

`RunBranchSelectionIntegrity.kt` has no imports of its own, but it is declared over `RunEntity`, so
the file will not compile without that type. `data/local/RunEntity.kt` also carries the Room
`@Entity` catalogue entry, whose `ForeignKey(entity = ChatEntity::class)` pulls in another entity
that cannot compile host-side.

* `agora_extract run-entity` copies the `data class RunEntity` declaration — **including its `init`
  invariants** — byte for byte into `build/RunEntity.kt`, and fails loudly if the declaration
  changes shape.
* `PlatformStubs.kt` supplies the single Room marker (`@PrimaryKey`) the declaration uses. Room is
  a persistence framework, i.e. a platform concern, and the marker has no runtime behaviour the
  policy can observe.
* `model/RuntimeIdentity.kt` is compiled verbatim (it has no imports at all) for `RunStatus` and
  `RunEndReason`.

The driver builds every Run as a terminal `COMPLETED` entity, which satisfies the invariant
`(activeSlot == 1) == !status.isTerminal` with `activeSlot = null`. The policy itself reads only
`id` and `parentRunId`, which is exactly the boundary the C++ port models as `RunRef`.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 27 frozen cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `RunBranchSelectionTraceDump.kt` | The stand driver |
| `agora_extract run-entity` | Lifts `RunEntity` out of `RunEntity.kt` verbatim |
| `PlatformStubs.kt` | Stand-local Room `@PrimaryKey` marker |
| `run_golden.sh` | Compiles and regenerates or verifies the traces |
| `agora_scenarios run-branch-selection` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

The projection is only a selection: dropping an invalid entry never deletes a Run or a message, and
with no explicit selection the caller's deterministic branch fallback applies. The traces pin the
validation itself:

* **the child must exist** — a selection whose child Run is gone is dropped;
* **the parent key is validated by kind**:
  * a **null** parent key matches only a child whose `parentRunId` is null;
  * a **non-null** parent key must name an existing Run, must not equal the child id (a
    **self-edge** is dropped even when the entity claims to be its own parent), and must equal the
    child's `parentRunId`;
* **a null key is not the empty string** — a child with `parentRunId = ""` is *not* a root, and a
  Run whose id is `""` is a real parent for a selection keyed `""`;
* **an empty selection stays empty** (it is not repaired into a root selection) and an empty run
  list drops every selection;
* **order survives**: the result is a `linkedMapOf`, so kept selections appear in input order, and
  a repeated selection key keeps its **first position** and its **last value**;
* **`associateBy(RunEntity::id)` keeps the last entity** for a duplicated run id. Renaming a run
  list's final duplicate to a root (or to another parent) flips which selection survives;
* unrelated Runs in the list change nothing.

## Scenario format (`scenarios/*.json`)

```json
{"name": "duplicate_run_ids_last_wins",
 "op": "retainValidEdges",
 "selections": [["p1", "c"], ["p2", "c"]],
 "runs": [{"id": "p1", "parentRunId": null},
          {"id": "p2", "parentRunId": null},
          {"id": "c", "parentRunId": "p1"},
          {"id": "c", "parentRunId": "p2"}]}
```

`selections` is a list of `[parent-or-null, child]` pairs, in map insertion order; a repeated key
behaves like `Map.put`. `runs` is a list of `{id, parentRunId}` objects, in list order; a repeated
id behaves like `associateBy` (last entity wins).

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|retainValidEdges
selections|<encoded pairs joined by |>
runs|<encoded runs joined by |>
out|map|<encoded kept pairs joined by |>
```

An id is encoded as `s:` + escaped id, and a null id as `n`, so the null key and an id literally
spelled `n` cannot collide. Escaping: `\` `|` `;` `=` `\n` `\r` `\t` →
`\\` `\p` `\s` `\e` `\n` `\r` `\t`.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios run-branch-selection core/tools/golden/run-branch-selection                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` (including the extracted `RunEntity.kt`)
before each run, so runs are idempotent.
