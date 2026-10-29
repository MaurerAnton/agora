# memory-manager golden stand

Parity pin for the native port of the memory catalog (`data/MemoryManager.kt` plus its
`data/DescriptionMetadataStore.kt`). Every scenario under `scenarios/` runs through the **real
Kotlin sources compiled verbatim from the app tree** and the canonical trace is frozen under
`golden/`; the C++ port (`agora::core::memory::MemoryManager`, test
`core/tests/memory_manager_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 32 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `MemoryManagerTraceDump.kt` | The stand driver (calls the real `MemoryManager`) |
| `PlatformStub.kt` | `android.content.Context` reduced to `filesDir` |
| `StateFlowStub.kt` | `kotlinx.coroutines.flow.StateFlow` as an aliasing counter |
| `run_golden.sh` | Compiles the app sources and regenerates or verifies the traces |
| `agora_scenarios memory-manager` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

The app's `MemoryManager` is a file-backed catalog with an active-memory scratch file, per-file
descriptions and revision counters:

* **Active memory** (`active_memory.md`, next to `memory_db/`) — `replace` / `append` / `prepend` /
  `patch`. Patch requires a non-blank `old_string` and exactly one occurrence; both failures are
  pinned messages.
* **Catalog files** (`memory_db/**.md`) — `create` / `read` / `edit` / `delete` / `list`. `edit`
  combines replace-content, patch, rename and description in one call, and its validation order
  (missing file → mutually exclusive → at-least-one → rename conflict → empty `old_string` →
  occurrence count) is part of the contract.
* **Descriptions** (`memory_db/memory_meta.json`) — written through `DescriptionMetadataStore`,
  which stages a `.tmp`, fsyncs, then renames via `.bak` so an interrupted write is recoverable.
  The serialized form is kotlinx `prettyPrint` over a **sorted** map.
* **Results and errors** — every method returns the Kotlin result string (`Created <relpath>`,
  `Updated <name>`, `Deleted <name>`, ...) or throws with the Kotlin `require` message. Both are
  part of the pinned trace; the driver records errors as an `error|<message>` line.
* **Revisions** — `activeMemoryRevision` bumps on every active-memory write, `catalogRevision` on
  every catalog/metadata mutation.

Only the two Android/coroutines platform types are substituted, by `PlatformStub.kt` and
`StateFlowStub.kt`; nothing in the stand duplicates the logic under test.

## Scenario format (`scenarios/*.json`)

```json
{ "name": "...", "mode": "...", "...mode-specific fields...": "..." }
```

`name` must equal the file stem. Common fields:

| Field | Meaning |
| --- | --- |
| `file_name` | the memory file the scenario operates on |
| `content_param` | create payload / edit content |
| `setup_content` | pre-creates `file_name` before the operation |
| `setup_target_name`, `setup_target_content` | pre-create a second file (e.g. rename conflicts) |
| `active_seed` | pre-seeds active memory before the operation |
| `old_string`, `new_string`, `new_name`, `description`, `meta_json`, `mode_param` | mode-specific |

Modes: `active`, `description_get`, `description_set`, `list`, `meta`, `meta_save`, `read`,
`create`, `edit`, `delete`, `revision`.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
mode|<mode>
result|<escaped result message>     (active, create, edit, delete)
active|<escaped active memory>      (active)
description|<escaped text>          (description_get)
ok                                  (description_set)
count|<n>                           (list)
file|<name>|<description>           (list, one line per file)
json|<escaped JSON>                 (meta, meta_save)
content|<escaped text>              (read)
error|<escaped message>             (any failing operation)
active|<revision>                   (revision)
catalog|<revision>                  (revision)
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`.

## Scope and boundaries

* Android `Context` is reduced to the one value it contributes: the files directory.
* `StateFlow` is only ever read as a counter here (never collected), so an aliasing value holder
  is a faithful substitute.
* Transport is out of scope; the stand exercises the file-system layer only.
* Traces must not contain absolute paths — `delete` reports `file.name` while `create` reports the
  `memory_db`-relative path, exactly as the Kotlin does.

## Regenerating

```bash
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios memory-manager core/tools/golden/memory-manager                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; `run_golden.sh` wipes the
per-scenario working directories first so the run is idempotent.
