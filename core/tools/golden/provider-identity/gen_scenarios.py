#!/usr/bin/env python3
"""Generates the scenario corpus for the provider-identity golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json and drops any stale file.
Freeze the golden traces afterwards with run_golden.sh --freeze.

Two traps this corpus is built to avoid, because an earlier revision fell into both:

  * The stable-id pattern is strict -- `custom-provider-` followed by 8-4-4-4-12 lowercase hex,
    with a version nibble in 1..5 and a variant nibble in 89ab. An id that does not match it is
    not "unstable input", it is *not an id at all*: `normalize` ignores it and mints a new one by
    calling the real UUID.randomUUID(). Scenarios that meant to exercise a passed-in generator were
    silently recording random ids, so every `uid()` below is pattern-valid.
  * `replaceStableIdsForDisplay` only rewrites text that matches that same pattern, so feeding it
    `custom-provider-abc123` asserted nothing at all.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, mode, **kw):
    out = {"name": name, "mode": mode}
    out.update(kw)
    ALL.append(out)


def uid(n):
    """A stable id that satisfies STABLE_ID_PATTERN: version 4, variant 8, all lowercase hex."""
    assert 0 <= n < 0x1000
    return f"custom-provider-{n:08x}-{n:04x}-4{n:03x}-8{n:03x}-{n:012x}"


# ---- legacyId -------------------------------------------------------------------------------
for name in [
    "plain", "uppercase", "with_spaces", "unicode", "special_chars", "very_long", "blank",
]:
    case(f"legacyId_{name}", "legacyId", providerName=f"test {name}")

# ---- replaceStableIdsForDisplay -------------------------------------------------------------
case("replaceIds_basic", "replaceIds", text=f"{uid(1)} called", replacement="PROV")
case("replaceIds_multiple", "replaceIds", text=f"a->{uid(1)} b->{uid(2)}", replacement="PROV")
case("replaceIds_no_ids", "replaceIds", text="no ids here", replacement="X")
case("replaceIds_multiple_in_word", "replaceIds", text=f"id{uid(1)}id", replacement="X")
case("replaceIds_overlapping", "replaceIds", text=f"{uid(1)}{uid(2)}", replacement="PROV")
case("replaceIds_empty", "replaceIds", text="", replacement="X")
# The pattern is applied case-insensitively, so an upper-case id is still redacted.
case("replaceIds_uppercase", "replaceIds", text=f"{uid(3).upper()} tail", replacement="PROV")
# A malformed neighbour is left alone: the pattern is not anchored, but it still has to match.
case("replaceIds_keeps_near_misses", "replaceIds",
     text="custom-provider-abc123 def45600-0000-4000-8000-000000000000", replacement="X")
# A version nibble outside 1..5 is a different UUID kind and must survive untouched.
case("replaceIds_skips_wrong_version", "replaceIds",
     text="custom-provider-00000001-0001-6000-8000-000000000001", replacement="X")

# ---- remapProviderReference -----------------------------------------------------------------
case("remap_no_change", "remap", modelId="custom-provider-abc123:model", migrations={})
case("remap_exact", "remap", modelId="custom-provider-abc123:model",
     migrations={"custom-provider-abc123": "new-id"})
case("remap_longest_prefix", "remap", modelId="custom-provider-abc123:model",
     migrations={"custom-provider-abc": "x", "custom-provider-abc123": "y"})
case("remap_no_prefix", "remap", modelId="custom-provider-abc123:model", migrations={"other": "x"})
case("remap_empty", "remap", modelId="model", migrations={})
case("remap_provider_prefix_only", "remap", modelId="custom-provider-abc123",
     migrations={"custom-provider-abc123": "new"})
# The match must end on the ':' separator, so a longer shared prefix does not match.
case("remap_requires_separator", "remap", modelId="custom-provider-abc123extra:model",
     migrations={"custom-provider-abc123": "new"})
# An empty legacy key cannot be a prefix.
case("remap_ignores_empty_legacy", "remap", modelId=":model", migrations={"": "new"})

# ---- canonicalCustomModelId -----------------------------------------------------------------
case("canonical_plain", "canonical", modelId="custom-provider-abc:model",
     providers=[{"name": "A", "id": "custom-provider-abc", "legacyNames": ["old-a"]}])
case("canonical_legacy_remap", "canonical", modelId="custom-provider-old:model",
     providers=[{"name": "A", "id": "custom-provider-new", "legacyNames": ["custom-provider-old"]}])
case("canonical_no_match", "canonical", modelId="custom-provider-x:model",
     providers=[{"name": "A", "id": "custom-provider-abc", "legacyNames": ["old-a"]}])
case("canonical_provider_name", "canonical", modelId="providerA:model",
     providers=[{"name": "providerA", "id": "id-A", "legacyNames": []}])
case("canonical_legacy_preferred", "canonical", modelId="custom-provider-old:model",
     providers=[{"name": "A", "id": "new-id", "legacyNames": ["custom-provider-old"]}])

# ---- remapModelPreferenceKeys ---------------------------------------------------------------
case("prefs_no_migration", "preferenceKeys", aliases={"model1": "a"}, migrations={})
case("prefs_migration", "preferenceKeys", aliases={"custom-provider-a:model1": "a"},
     migrations={"custom-provider-a": "new-id"})
case("prefs_partial_overlap", "preferenceKeys",
     aliases={"custom-provider-a:model1": "a", "custom-provider-b:model2": "b"},
     migrations={"custom-provider-a": "new-a"})
# A remapped key that already exists must keep the alias it had: the second pass uses
# putIfAbsent precisely so a migration cannot overwrite a live preference.
case("prefs_collision_keeps_existing", "preferenceKeys",
     aliases={"custom-provider-old:model1": "a", "new-a:model1": "b"},
     migrations={"custom-provider-old": "new-a"})

case("prefs_chain", "preferenceKeys",
     aliases={"custom-provider-old:model": "a"},
     migrations={"custom-provider-old": "mid", "mid": "new"})

# ---- normalize --------------------------------------------------------------------------------
def p(name, type="conch", id="", legacy=None):
    d = {"name": name, "type": type}
    if id:
        d["id"] = id
    if legacy is not None:
        d["legacyNames"] = list(legacy)
    return d


case("normalize_new_empty", "normalize", providers=[], occupiedIds=[], newIds=[])
case("normalize_no_change", "normalize", providers=[{"name": "A", "id": uid(1)}],
     occupiedIds=[], newIds=[])
case("normalize_blank_id_generated", "normalize", providers=[p("A", "conch", "")],
     occupiedIds=[], newIds=[uid(1)])
case("normalize_unstable_id_generated", "normalize", providers=[p("A", "conch", "unstable")],
     occupiedIds=[], newIds=[uid(2)])
case("normalize_blank_name", "normalize", providers=[p("   ", "conch", "id1")],
     occupiedIds=[], newIds=[])
case("normalize_legacy_name_carry", "normalize", providers=[p("A", "conch", "", legacy=["old-a"])],
     occupiedIds=[], newIds=[])
case("normalize_legacy_names_filtered", "normalize",
     providers=[p("A", "conch", "stable-id", legacy=["old-a", "stable-id"])],
     occupiedIds=[], newIds=[])
case("normalize_occupied_skips", "normalize", providers=[p("A", "conch", "id-in-use")],
     occupiedIds=["id-in-use"], newIds=[uid(1)])
case("normalize_two_providers", "normalize",
     providers=[p("A", "conch", ""), p("B", "ssh", "")],
     occupiedIds=[], newIds=[uid(1), uid(2)])
case("normalize_conch_and_ssh", "normalize",
     providers=[p("Conch", "conch", ""), p("SSH", "ssh", "")],
     occupiedIds=[], newIds=[uid(1), uid(2)])
case("normalize_empty_name_conch", "normalize", providers=[p("", "conch", "")],
     occupiedIds=[], newIds=[])
case("normalize_ssh_only", "normalize", providers=[p("SSH", "ssh", "")], occupiedIds=[], newIds=[])
case("normalize_occupied_stable_id", "normalize", providers=[p("A", "conch", "stable-id")],
     occupiedIds=["stable-id"], newIds=[uid(1)])
case("normalize_new_id_in_occupied", "normalize", providers=[p("A", "conch", "")],
     occupiedIds=[uid(1)], newIds=[uid(2)])
case("normalize_three_providers", "normalize",
     providers=[p("A", "conch", ""), p("B", "ssh", ""), p("C", "conch", "")],
     occupiedIds=[], newIds=[uid(1), uid(2), uid(3)])
case("normalize_legacy_name_carry_with_id", "normalize",
     providers=[p("A", "conch", "id1", legacy=["old-a"])], occupiedIds=[], newIds=[])
case("normalize_unicode_name", "normalize", providers=[p("Провайдер", "conch", "")],
     occupiedIds=[], newIds=[uid(1)])
case("normalize_name_with_special_chars", "normalize", providers=[p("a,b c;d", "conch", "")],
     occupiedIds=[], newIds=[uid(1)])

# The pattern is case-insensitive and trims before matching, so both of these are retained as-is.
case("normalize_uppercase_stable_id_retained", "normalize",
     providers=[p("A", "conch", uid(4).upper())], occupiedIds=[], newIds=[])
case("normalize_stable_id_is_trimmed", "normalize",
     providers=[p("A", "conch", f"  {uid(5)}  ")], occupiedIds=[], newIds=[])

# A version-6 or variant-c UUID is not a stable custom-provider id, so the policy regenerates it.
case("normalize_wrong_version_regenerated", "normalize",
     providers=[p("A", "conch", "custom-provider-00000001-0001-6000-8000-000000000001")],
     occupiedIds=[], newIds=[uid(6)])
case("normalize_wrong_variant_regenerated", "normalize",
     providers=[p("A", "conch", "custom-provider-00000001-0001-4000-c000-000000000001")],
     occupiedIds=[], newIds=[uid(7)])

# occupiedIds is filtered through isStableId too, so a malformed reservation cannot block an id.
case("normalize_malformed_occupied_is_ignored", "normalize",
     providers=[p("A", "conch", "")],
     occupiedIds=["custom-provider-abc123"], newIds=[uid(8)])

# Two providers asking for the same id: the first keeps it, the second is regenerated.
case("normalize_duplicate_id_regenerated", "normalize",
     providers=[p("A", "conch", uid(9)), p("B", "ssh", uid(9))],
     occupiedIds=[], newIds=[uid(10)])

# generateUniqueId retries with CustomProviderIdentityPolicy.newId(), i.e. a real
# UUID.randomUUID(), and a verbatim stand cannot freeze that. The retry path is therefore NOT
# golden-pinned here; it is unit-pinned in core/tests/provider_identity_parity_test.cpp. An earlier
# scenario pretended to cover it but its ids did not match the pattern, so nothing was exercised
# and the frozen trace simply captured one lucky random draw.


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for stale in sorted(os.listdir(SCENARIOS_DIR)):
        if stale.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, stale))
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
