# legacy-run-backfill golden stand

Parity pin for `data/local/migration/LegacyRunBackfillPlanner.kt` — the v16 message forest → v17
Run conversion used by `Migration16To17`. The planner compiles **verbatim from the app tree**; the
four model enums it needs (`Participant`, `MessageStatus`, `RunStatus`, `RunEndReason`) are lifted
declaration-for-declaration by `agora_extract enums-legacy`. The shipped port must reproduce the frozen
traces byte for byte (`core/tests/legacy_run_backfill_parity_test.cpp`).

## What it plans

* every visible (`USER`, non-`tool_`/`result_`) message starts a Run with a deterministic UUID v3
  id over `legacy-run:<conversation>:user:<message>`;
* model and synthetic descendants stay in that Run until the next visible user boundary;
* rows whose parent is missing (or that lead with a model/synthetic row) seed an **orphan** Run
  (`legacy-run:<conversation>:orphan:<message>`) and are marked `legacyAmbiguous`;
* runs keep input-topology order; the status is `FAILED/PROVIDER_ERROR` when any row is an ERROR
  participant/status, `STOPPED/PROCESS_RECOVERED` for unfinished statuses, otherwise
  `COMPLETED/MODEL_COMPLETED`;
* historical model siblings are grouped losslessly into one Run and marked `legacyAmbiguous`
  instead of being split by cloning the shared input;
* `selectedRunBranches` migrates legacy nullable message-level selections to Run-level selections,
  walking each Run boundary and choosing the selected (else the last visible) child.

## What is pinned

* **determinism** — UUID v3 ids (JDK `nameUUIDFromBytes`), assignment sequences per Run, consumed
  pass only for visible user rows, `conversationId`/parent/boundary fields.
* **ordering** — Kahn topology over `(timestamp, id)`; input-order tie buckets; corrupt parent
  cycles retained in `(timestamp, id)` order and seeded as ambiguous orphan Runs.
* **guards** — blank `conversationId`, duplicate message ids (both `require` messages), empty
  forests, orphan models/synthetic users, nested boundaries under tool rows, synthetic flag
  overrides.
* **statuses** — every message status plus ERROR participant, error precedence over stopped.
* **selection** — root and per-boundary choices, missing selections, default last-visible child,
  synthetic-only candidate fallback, same-Run boundary walks, cycle safety.

## Documented boundaries

* Message ids are ASCII (UUIDs, `tool_`/`result_` prefixes); both Kotlin UTF-16 order and C++
  byte order agree on ASCII.
* `isSynthetic` defaults to the `tool_`/`result_` id convention exactly like the Kotlin default.
* `timestamp` is a Kotlin `Long`; the port parses the full signed range.
* Java's `UUID.nameUUIDFromBytes` MD5 is shared with `provider_identity` in `core/src/uuid_v3.cpp`.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`agora_scenarios legacy-run-backfill` rewrites `scenarios/` deterministically (42 cases); rerun
`./run_golden.sh --freeze` afterwards.
