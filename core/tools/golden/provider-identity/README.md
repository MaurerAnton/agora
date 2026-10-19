# provider-identity golden stand

Parity pin for the native port of `data/CustomProviderIdentity.kt` and the
`data/CustomProviderNamePolicy.kt` it depends on. Every scenario under `scenarios/` runs through the
**real Kotlin sources compiled from the app tree** and the canonical trace is frozen under
`golden/`; the C++ port (`agora::core::provider_identity`, test
`core/tests/provider_identity_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 58 frozen cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ProviderIdentityTraceDump.kt` | The stand driver |
| `extract_identity.py` | Lifts the identity policy out of `CustomProviderIdentity.kt` |
| `PlatformStubs.kt` | Platform-type stand-ins the extracted sources need |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

**`legacyId`** is a **UUID v3**: MD5 over `"agora/custom-provider/legacy/" + trim(name).lowercase()`
with the version and variant nibbles overwritten. It is stable for upgrading a legacy name-keyed
config, and by construction it can never be renamed — the id does not mention the current name.

**`isStableId`** is stricter than "looks like an id": `custom-provider-` followed by 8-4-4-4-12 hex,
a **version nibble in 1..5** and a **variant nibble in 89ab**, case-insensitive, trimmed first. A
version-6 or variant-c UUID is not a custom provider id, and `normalize` will regenerate it.

**`replaceStableIdsForDisplay`** matches that pattern anywhere inside a string, so an id embedded in
a word is redacted and a near miss such as `custom-provider-abc123` is left alone.

**`normalize`** the interesting cases are all about what is *not* rewritten:

* An existing stable id is retained **verbatim**, including its original case and only after
  trimming — `normalize_uppercase_stable_id_retained` keeps `CUSTOM-PROVIDER-…`, and
  `normalize_stable_id_is_trimmed` strips the padding.
* A retained id produces **no migration**, because the provider was not renamed.
* A regenerated id adds the provider's **name** as a legacy reference, which is what makes the
  persisted migration list complete.
* Two providers asking for the same id: the **first** keeps it, the second is regenerated — and only
  the second gets a migration (`normalize_duplicate_id_regenerated`).
* `occupiedIds` is filtered through `isStableId` too, so a malformed reservation cannot block an id.
* Reserved names and duplicate names are quarantined by `sanitize`; the first accepted spelling wins
  and rejected configs keep their original, untrimmed name so name-keyed settings are never
  rewritten.

**`remapProviderReference`** rewrites onto the **longest** migrated prefix, and the prefix must be
followed by `:` — otherwise a shared prefix would swallow a longer provider id
(`remap_requires_separator`). Ties keep the first entry in map order.

**`remapModelPreferenceKeys`** is two passes with `putIfAbsent`, so a migration can never overwrite
an alias that already exists under the new reference (`prefs_collision_keeps_existing`).

**`canonicalCustomModelId`** maps each provider's legacy names *and* its name onto
`providerId = id.takeIf(::isStableId) ?: name`, later entries winning.

## Scenario format (`scenarios/*.json`)

```json
{"name": "...", "mode": "normalize",
 "providers": [{"name": "A", "type": "conch"}],
 "occupiedIds": [], "newIds": ["custom-provider-00000001-0001-4001-8001-000000000001"]}
```

`mode` is one of `legacyId`, `replaceIds`, `remap`, `canonical`, `preferenceKeys`, `normalize`.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
mode|<mode>
legacyId|<id>            # legacyId
text|<result>            # replaceIds
remap|<result>           # remap
model|<result>           # canonical
count|<n> / key|<k>|<v>  # preferenceKeys
providers|<n> / provider|<i>|<name>|<id>|<legacyNames joined by ;>
migrations|<n> / migration|<i>|<legacy>|<providerId>   # normalize
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`.

## Determinism, and why the ids in this corpus are pattern-valid

`normalize` calls the policy's own `newId()` — a real `UUID.randomUUID()` — whenever a candidate id
fails `isStableId` or is already taken. An earlier revision of this corpus fed it ids such as
`custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa`, which **do not match the pattern**. Every one of
those scenarios therefore fell through to the random generator, and the frozen traces recorded one
lucky draw each: the stand could never be re-verified, and did not test what its names claimed.
`gen_scenarios.py` now emits only pattern-valid ids via `uid()`, and the traces are reproducible —
`run_golden.sh` is run twice in CI-adjacent verification precisely because that property is not
self-evident.

The same trap applies to `replaceIds`: ids that do not match the pattern are not redacted, so those
six scenarios used to assert nothing at all.

## Scope and boundaries

* **The retry path is unit-pinned, not golden-pinned.** `generateUniqueId` retries with the policy's
  own `newId()` for attempts after the first, which a verbatim stand cannot freeze. The port takes
  that entropy as an explicit `RandomIdSource` parameter rather than calling a hidden generator, and
  `core/tests/provider_identity_parity_test.cpp` pins the behaviour with an injected source: first
  candidate taken, first candidate malformed, and the retained-id path that must never consult a
  generator. The retired `normalize_max_retry_exhausted` scenario was removed for this reason — it
  could not have been deterministic, and in practice its ids were malformed, so it exercised no
  retry at all.
* **`lowercase()` and `equals(ignoreCase = true)` are ASCII folds**, the same boundary the other
  ports in this tree document. Every pinned input is ASCII; a Unicode case-fold table would be a
  separate piece of work, not a silent widening here.
* The driver passes the scenario's `newIds` in order and falls back to a fixed sentinel. A scenario
  that unexpectedly needed a retry would produce a *wrong* trace and fail, rather than quietly
  matching a random draw.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`gen_scenarios.py` deletes stale scenario files first, so a removed case cannot leave a trace behind
that silently keeps passing.
