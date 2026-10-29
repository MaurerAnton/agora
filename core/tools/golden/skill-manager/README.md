# skill-manager golden stand

Parity pin for the native port of the skill catalog (`data/SkillManager.kt` plus its shared
`data/DescriptionMetadataStore.kt`). Every scenario under `scenarios/` runs through the **real
Kotlin sources compiled verbatim from the app tree** and the canonical trace is frozen under
`golden/`; the C++ port (`agora::core::skill::SkillManager`, test
`core/tests/skill_manager_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 35 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `SkillManagerTraceDump.kt` | The stand driver (calls the real `SkillManager`) |
| `PlatformStub.kt` | `android.content.Context` reduced to `filesDir` |
| `StateFlowStub.kt` | `kotlinx.coroutines.flow.StateFlow` as an aliasing counter |
| `run_golden.sh` | Compiles the app sources and regenerates or verifies the traces |
| `agora_scenarios skill-manager` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

The app's `SkillManager` is a **flat** file-backed catalog next to the memory one
(`<files_dir>/skill_db` + `skill_meta.json`):

* **Listing** — `listFiles()` keeps only entries whose extension is exactly `md` (the case
  sensitive `it.extension == "md"` rule; a raw `notes.txt` neighbour is excluded) and sorts by
  name. Descriptions come from `skill_meta.json` keyed by the bare file name.
* **`catalog()`** — the `<available_skills>` preload block: the exact header sentence, `- name`
  lines in list order, `- name: description` only when the description is non-blank, with `'\n'`
  folded to a space and the result trimmed; empty catalog → `""`.
* **The `.md` rule and sanitisation** — `resolveFile` replaces `/` and `\` with `_` and appends
  `.md` unless the name already ends with it (`create` from `notes` yields `Created notes.md`,
  from `nested/thing` yields `Created nested_thing.md`). The resolved file's canonical parent
  must be exactly `skill_db`.
* **Catalog files** — `create` / `read` / `edit` / `delete` over `skill_db/**.md`. `edit`
  combines replace-content, patch, rename and description in one call, and its validation order
  (missing file → mutually exclusive → at-least-one → rename conflict → empty `old_string` →
  occurrence count) is part of the contract, including `No changes made.` when nothing changes.
* **Descriptions** (`skill_meta.json`) — written through `DescriptionMetadataStore`, which
  stages a `.tmp`, fsyncs, then renames via `.bak` so an interrupted write is recoverable.
  The serialized form is kotlinx `prettyPrint` over a **sorted** map. A corrupt metadata file
  fails reads with `Unable to parse skill_meta.json; metadata was not modified`; invalid JSON on
  `saveMetaJson` fails with `Invalid metadata JSON`.
* **Results and errors** — every method returns the Kotlin result string (`Created <name>`,
  `Updated <name>`, `Deleted <name>`, ...) or throws with the Kotlin `require` message. Both are
  part of the pinned trace; the driver records errors as an `error|<message>` line.
* **Revisions** — `catalogRevision` bumps on every catalog/metadata mutation.

Only the two Android/coroutines platform types are substituted, by `PlatformStub.kt` and
`StateFlowStub.kt`; nothing in the stand duplicates the logic under test.

## Scenario format (`scenarios/*.json`)

```json
{ "name": "...", "mode": "...", "...mode-specific fields...": "..." }
```

`name` must equal the file stem. Common fields:

| Field | Meaning |
| --- | --- |
| `file_name` | the skill file the scenario operates on (the `.md` rule applies) |
| `content_param` | create payload / edit content |
| `setup_content`, `setup_description` | pre-creates `file_name` (with description) before the operation |
| `setup_target_name`, `setup_target_content` | pre-create a second file (e.g. rename conflicts) |
| `setup_raw_name`, `setup_raw_content` | write a raw neighbour file into `skill_db` (extension filter) |
| `setup_raw_meta` | write a raw `skill_meta.json` (corrupt-metadata path) |
| `old_string`, `new_string`, `new_name`, `description`, `meta_json` | mode-specific |

Modes: `description_get`, `list`, `catalog`, `meta`, `meta_save`, `read`, `create`, `edit`,
`delete`, `revision`.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
mode|<mode>
description|<escaped text>          (description_get)
count|<n>                           (list)
file|<name>|<description>           (list, one line per file)
catalog|<escaped block>             (catalog)
catalog|<revision>                  (revision)
json|<escaped JSON>                 (meta, meta_save)
content|<escaped text>              (read)
result|<escaped result message>     (create, edit, delete)
error|<escaped message>             (any failing operation)
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`.

## Scope and boundaries

* Android `Context` is reduced to the one value it contributes: the files directory.
* `StateFlow` is only ever read as a counter here (never collected), so an aliasing value holder
  is a faithful substitute.
* The catalog is flat by design: unlike `MemoryManager` the skill manager never walks
  subdirectories and never prunes empty ones, and its messages carry the bare `file.name`.
* `isBlank`/`trim` are evaluated over ASCII whitespace; the pinned corpus uses ASCII
  descriptions and file names.
* Transport is out of scope; the stand exercises the file-system layer only.
* Traces contain no absolute paths: every message reports `file.name`.

## Regenerating

```bash
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios skill-manager core/tools/golden/skill-manager                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; `run_golden.sh` wipes the
per-scenario working directories first so the run is idempotent.
