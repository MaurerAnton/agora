#!/usr/bin/env python3
"""Generates the scenario corpus for the provider-identity golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
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


# ---- legacyId -------------------------------------------------------------------------------
for name in [
    "legacyId_plain", "legacyId_uppercase", "legacyId_with_spaces", "legacyId_unicode",
    "legacyId_special_chars", "legacyId_very_long", "legacyId_blank",
]:
    case(f"legacyId_{name}", "legacyId", providerName=f"test {name}")

# ---- replaceStableIdsForDisplay -------------------------------------------------------------
case("replaceIds_basic", "replaceIds", text="custom-provider-abc123-def456 called", replacement="PROV")
case("replaceIds_multiple", "replaceIds", text="a->custom-provider-abc123 b->custom-provider-def456", replacement="PROV")
case("replaceIds_no_ids", "replaceIds", text="no ids here", replacement="X")
case("replaceIds_multiple_in_word", "replaceIds", text="idcustom-provider-abc123id", replacement="X")
case("replaceIds_overlapping", "replaceIds", text="custom-provider-abc123custom-provider-def456", replacement="PROV")
case("replaceIds_empty", "replaceIds", text="", replacement="X")

# ---- remapProviderReference -----------------------------------------------------------------
case("remap_no_change", "remap", modelId="custom-provider-abc123:model", migrations={})
case("remap_exact", "remap", modelId="custom-provider-abc123:model", migrations={"custom-provider-abc123": "new-id"})
case("remap_longest_prefix", "remap", modelId="custom-provider-abc123:model", migrations={"custom-provider-abc": "x", "custom-provider-abc123": "y"})
case("remap_no_prefix", "remap", modelId="custom-provider-abc123:model", migrations={"other": "x"})
case("remap_empty", "remap", modelId="model", migrations={})
case("remap_provider_prefix_only", "remap", modelId="custom-provider-abc123", migrations={"custom-provider-abc123": "new"})

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
case("prefs_chain", "preferenceKeys",
     aliases={"custom-provider-old:model": "a"},
     migrations={"custom-provider-old": "mid", "mid": "new"})

# ---- normalize --------------------------------------------------------------------------------
def p(name, type="conch", id="", legacy=None):
    d = {"name": name, "type": type}
    if id: d["id"] = id
    if legacy is not None: d["legacyNames"] = list(legacy)
    return d

case("normalize_new_empty", "normalize", providers=[], occupiedIds=[], newIds=[])
case("normalize_no_change", "normalize", providers=[{"name": "A", "id": "stable-id-123"}],
     occupiedIds=[], newIds=[])
case("normalize_blank_id_generated", "normalize", providers=[p("A", "conch", "")],
     occupiedIds=[], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"])
case("normalize_unstable_id_generated", "normalize", providers=[p("A", "conch", "unstable")],
     occupiedIds=[], newIds=["custom-provider-bbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"])
case("normalize_blank_name", "normalize", providers=[p("   ", "conch", "id1")], occupiedIds=[], newIds=[])
case("normalize_legacy_name_carry", "normalize", providers=[p("A", "conch", "", legacy=["old-a"])],
     occupiedIds=[], newIds=[])
case("normalize_legacy_names_filtered", "normalize", providers=[p("A", "conch", "stable-id", legacy=["old-a", "stable-id"])],
     occupiedIds=[], newIds=[])
case("normalize_occupied_skips", "normalize", providers=[p("A", "conch", "id-in-use")],
     occupiedIds=["id-in-use"], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"])
case("normalize_two_providers", "normalize", providers=[p("A", "conch", ""), p("B", "ssh", "")],
     occupiedIds=[], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
                              "custom-provider-bbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"])
case("normalize_conch_and_ssh", "normalize", providers=[p("Conch", "conch", ""), p("SSH", "ssh", "")],
     occupiedIds=[], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
                              "custom-provider-bbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"])
case("normalize_empty_name_conch", "normalize", providers=[p("", "conch", "")], occupiedIds=[], newIds=[])
case("normalize_ssh_only", "normalize", providers=[p("SSH", "ssh", "")], occupiedIds=[], newIds=[])
case("normalize_occupied_stable_id", "normalize", providers=[p("A", "conch", "stable-id")],
     occupiedIds=["stable-id"], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"])
case("normalize_new_id_in_occupied", "normalize", providers=[p("A", "conch", "")],
     occupiedIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"], newIds=[
        "custom-provider-bbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb"])
case("normalize_three_providers", "normalize", providers=[p("A", "conch", ""), p("B", "ssh", ""), p("C", "conch", "")],
     occupiedIds=[], newIds=[
         "custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
         "custom-provider-bbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb",
         "custom-provider-cccc-cccc-cccc-cccc-cccccccccccc",
     ])
case("normalize_max_retry_exhausted", "normalize", providers=[p("A", "conch", "")],
     occupiedIds=[f"custom-provider-{i:04x}-{i:04x}-{i:04x}-{i:04x}-{i:012x}" for i in range(101)],
     newIds=[f"custom-provider-{i:04x}-{i:04x}-{i:04x}-{i:04x}-{i:012x}" for i in range(101, 102)])
case("normalize_legacy_name_carry_with_id", "normalize",
     providers=[p("A", "conch", "id1", legacy=["old-a"])],
     occupiedIds=[], newIds=[])
case("normalize_unicode_name", "normalize", providers=[p("Провайдер", "conch", "")],
     occupiedIds=[], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"])
case("normalize_name_with_special_chars", "normalize", providers=[p("a,b c;d", "conch", "")],
     occupiedIds=[], newIds=["custom-provider-aaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"])


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()