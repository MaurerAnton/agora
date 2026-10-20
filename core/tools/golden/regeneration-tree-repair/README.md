# regeneration-tree-repair golden stand

Parity pin for `data/local/migration/RegenerationTreeRepairPlanner.kt` — the v17 Regenerate/Edit
repair. v17 did not store the initiating operation, so an exact repeated boundary payload is the
only durable discriminator: sibling Runs with the same parent Run, parent message and input
fingerprint form one group; the earliest sibling is the shared user anchor, later siblings are
inferred regenerations. `RegenerationTreeRepairPlanner.kt` compiles **verbatim from the app tree**;
`Participant` is lifted declaration-for-declaration from `ChatMessage.kt` by
`extract_participant.py`. The canonical trace is frozen under `golden/` and the shipped port must
reproduce it byte for byte (`core/tests/regeneration_tree_repair_parity_test.cpp`).

## What it repairs

For every group of two or more sibling Runs sharing the same parent Run, parent message and input
fingerprint:

1. the sibling ordered by `(startedAt, boundary timestamp, runId)` is the anchor;
2. every later sibling with a root assistant and leading cloned user inputs is inferred: its
   leading ordinary USER rows are deleted, its root assistant is reparented to the anchor user,
   and the Run becomes a child of the anchor Run;
3. retained rows are reindexed by `(runSequence, timestamp, id)` from zero;
4. the selection maps are migrated (`messageSelections` / `runSelections`) so the chosen output
   survives the rewrite, deleting entries that point at deleted rows and dropping self-edges;
5. Runs outside that exact shape are left untouched.

## What is pinned

* **grouping** — fingerprints, parent Run and parent message all participate; single-entry groups,
  missing Run records, missing root outputs and root-before-boundary orders do nothing.
* **ordering** — anchor selection by `startedAt` (missing Run = `Long.MAX_VALUE`), boundary
  timestamp and Run id, including reversed input order; repairs appended in group first-seen order.
* **shape** — synthetic `tool_`/`result_` rows and ERROR rows are never boundaries, roots or
  leading inputs, but synthetic retained MODEL rows are reparented and reindexed like any other.
* **selections** — selection by boundary message (must be a removed leading input) or by Run id,
  fallback to the latest reparented row, then to the root output; entries pointing at deleted rows
  are removed; the anchor self-edge is replaced by the original output and the self Run selection
  is dropped.
* **dedup contracts** — duplicate Run ids use the last record; a message id repaired twice keeps
  its first map position and takes the later value.
* **fingerprint** — `regenerationInputFingerprint` over `text\0imageCount\0attachment` with
  recursive `originalUri` removal, kotlinx re-serialization (string escaping, preserved number
  lexemes, last duplicate key wins) and SHA-256 lowercase hex; blank metadata normalizes to empty
  and invalid JSON is used raw.

## Documented boundaries

* Message and Run ids are ASCII in the app (UUIDs, `tool_`/`result_`/`compact_` prefixes); the port
  compares `std::string` bytes while Kotlin compares UTF-16 code units. Both orders match for
  ASCII, and no scenario uses non-ASCII ids.
* Attachment metadata is expected to be a JSON document kotlinx accepts. Documents with duplicate
  `originalUri` keys are sanitized by key removal (all occurrences), which matches kotlinx's
  last-wins parse for the sanitized result.
* `startedAt`/`timestamp`/`runSequence` are Kotlin `Long`; the port carries `long long` and parses
  the full signed range.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (53 cases); rerun
`./run_golden.sh --freeze` afterwards.
