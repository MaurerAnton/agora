# native-run-archive golden stand

Parity pin for `data/NativeRunArchivePolicy.kt`: terminalizing imported v3 Run rows, checking that
message -> Run ownership is complete, and producing a parent-before-child import order.

`NativeRunArchivePolicy.kt`, `model/RuntimeIdentity.kt` and `data/local/RunEntity.kt` compile
**verbatim from the app tree**. Only RunEntity's Room annotations (`@Entity`, `@PrimaryKey`,
`@ForeignKey`, `@Index`) and the `ChatEntity` table it names are stand-local platform stubs: they
carry no runtime behaviour the policy can observe.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 32 terminalize/ownership/order cases |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `NativeRunArchiveTraceDump.kt` | Driver: runs the policy, prints every observable field |
| `platform/RoomAnnotations.kt` | Stand-local Room annotation declarations |
| `platform/ChatEntity.kt` | Minimal `chats` table referenced by RunEntity's foreign key |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **`terminalize` status**: every stored status round-trips when terminal; `ACTIVE` and `STOPPING`
  become `STOPPED` with `PROCESS_RECOVERED`; an invalid or absent status becomes `FAILED`.
* **`terminalize` reason**: a nonterminal row ignores any stored reason. A terminal row uses a
  valid stored reason even when it disagrees with the status; a null/invalid reason falls back to
  `MODEL_COMPLETED` / `USER_STOPPED` / `PROVIDER_ERROR` by the terminal status.
* **`terminalize` timestamps**: `lastCheckpointAt = max(startedAt, lastCheckpointAt)` and
  `endedAt = max(endedAt ?? checkpoint, checkpoint)`; `stopRequestedAt` is carried through.
  `currentPass` is clamped at 0 and `activeSlot` is always null.
* **`hasCompleteOwnership`**: nonempty runs and a unique source run-id set; distinct message ids;
  every ownership row names an existing Run in the same conversation with a non-null,
  non-negative sequence; no repeated `(runId, sequence)`. An empty ownership list over nonempty
  runs is complete; duplicate run rows resolve like `associateBy` (last entity wins).
* **`orderByParent`**: a missing parent becomes a null parent marked `legacyAmbiguous`; cycles are
  broken by marking the smallest remaining `(startedAt, id)` as a root + ambiguous; each ready
  batch is sorted by `startedAt` then id, so a child with an earlier timestamp still waits for its
  parent. Duplicate run ids behave like `associateByTo(linkedMapOf())`: last value, first position.
  An empty input stays empty.

## Scenario format (`scenarios/*.json`)

```json
{"name": "order_cycle",
 "op": "orderByParent",
 "runs": [{"id": "a", "parentRunId": "c", "startedAt": 3},
          {"id": "b", "parentRunId": "a", "startedAt": 1},
          {"id": "c", "parentRunId": "b", "startedAt": 2}]}
```

Ops are `terminalize` (snapshot fields at the root), `hasCompleteOwnership` (`runs`,
`ownership`, `sourceRunIdsWereUnique`) and `orderByParent` (`runs`). Runs default to a terminal
`COMPLETED` row (`endedAt = startedAt + 1`, `MODEL_COMPLETED`); ownership rows default to
`conversationId = "conv-a"`.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|<operation>
<role>|<encoded record>[|<encoded record>...]
...
```

Each record is `field=value` pairs joined by `;`. Text is `n` when null or `s:` + escaped text;
longs are `n` or `l:<decimal>`; ints are `n` or `i:<decimal>`. Escaping: `\` `|` `;` `=` `\n`
`\r` `\t` -> `\\` `\p` `\s` `\e` `\n` `\r` `\t`. `terminalize` prints `snapshot` and `out`;
ownership prints `runs`, `ownership`, `sourceRunIdsWereUnique` and `out`; ordering prints `input`
and `out`. Every Run field is printed, so a port that mutates the wrong column is caught.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                         # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
