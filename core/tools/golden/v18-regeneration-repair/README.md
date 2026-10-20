# v18-regeneration-repair golden stand

Parity pin for `data/local/migration/V18RegenerationEdgeRepairPlanner.kt` — the forward repair for
the rejected first v17 → v18 data migration. `V18RegenerationEdgeRepairPlanner.kt` and the real
`util/Constants.kt` compile **verbatim from the app tree**; `Participant` is lifted
declaration-for-declaration from `ChatMessage.kt` by `extract_participant.py`. The canonical trace
is frozen under `golden/` and the shipped port must reproduce it byte for byte
(`core/tests/v18_regeneration_repair_parity_test.cpp`).

## What the repair does

The rejected migration already converted one Run into an output-only regeneration Run and
reparented its first assistant to the canonical user, but it could leave additional assistant
siblings pointing at the deleted cloned user. The planner repairs exactly that partial shape:

1. the child Run is marked legacy-ambiguous and its parent Run exists;
2. the parent Run still has an ordinary USER message at `runSequence == 0`;
3. the child Run already has an ordinary MODEL message rooted at that user;
4. only then every ordinary MODEL sibling whose `parentId` is not a known message id is reparented
   to the canonical user (nullable parents and existing parents are untouched).

## What is pinned

* **guard rails** — no legacy flag, no parent id, missing parent Run (even when orphan messages
  with that run id exist), no canonical user, a user at a non-zero sequence, tool_/result_ user
  ids, and a missing repaired root all produce no updates.
* **the repaired shape** — sibling reparenting only for ordinary MODEL rows with a non-null,
  dangling parent; existing parents, null parents, tool_/result_ ids and USER rows are kept.
* **ordering** — repairs are emitted in child-Run input order, siblings sorted by
  `(runSequence, id)`; duplicate run ids use the last record for the parent lookup; a duplicate
  message id across Runs keeps its first insertion position but takes the last value (the
  LinkedHashMap contract).
* **canonical user selection** — the minimum ordinary USER at sequence zero by id, ignoring MODEL
  rows even when their id sorts first.
* **repaired-root selection** — the root must be an ordinary MODEL whose parent is exactly the
  canonical user; tool_ roots, USER roots and null-parent roots do not count.
* **constants** — the four `util/Constants.kt` prefixes the ordinary filter and callers rely on.

## Documented boundaries

* Message and run ids are ASCII in the app (UUIDs and `tool_`/`result_`/`compact_` prefixes). The
  port compares `std::string` bytes; Kotlin compares UTF-16 code units. Both orders match for
  ASCII, and no golden scenario uses non-ASCII ids.
* `runSequence` is a Kotlin `Long`; the port carries `long long` and parses the full signed range.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (26 cases); rerun
`./run_golden.sh --freeze` afterwards.
