#!/usr/bin/env python3
"""Generates the custom-model-references scenarios. Deterministic; rerun after editing.

The interesting parts are not the happy path but the container semantics the three helpers inherit
from Kotlin: `buildSet` keeps insertion order and drops a duplicate of the replacement, a map
rename through `remove` + `put` moves an `old == new` key to the end, an existing target key keeps
its position, and `isNotBlank()` is `Char.isWhitespace`, so a no-break space counts as blank while
a zero-width space is content.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

NBSP = "\u00a0"
ZWSP = "\u200b"

ALL = []


def case(name, op, **kw):
    entry = {"name": name, "op": op}
    entry.update(kw)
    ALL.append(entry)


# ---- Set<String>.replaceModelReference -------------------------------------------------------
case("set_replaces_existing", "set", oldModelId="a", newModelId="b", items=["a", "c"])
case("set_removes_when_new_is_null", "set", oldModelId="a", newModelId=None, items=["a", "b"])
case("set_absent_old_is_unchanged", "set", oldModelId="x", newModelId="y", items=["a", "b"])
case("set_absent_old_with_null_new_is_unchanged", "set", oldModelId="x", newModelId=None,
     items=["a", "b"])
case("set_old_equals_new_keeps_place", "set", oldModelId="a", newModelId="a", items=["a", "b"])
case("set_new_already_present_dedupes", "set", oldModelId="a", newModelId="c", items=["a", "c"])
case("set_new_present_before_old_dedupes", "set", oldModelId="a", newModelId="c", items=["c", "a"])
case("set_removes_only_element", "set", oldModelId="a", newModelId=None, items=["a"])
case("set_empty_input", "set", oldModelId="a", newModelId="b", items=[])
case("set_empty_old_matches_empty_element", "set", oldModelId="", newModelId="x",
     items=["", "a"])
case("set_new_is_empty_string", "set", oldModelId="a", newModelId="", items=["a", "b"])
case("set_duplicate_input_entries_dedupe", "set", oldModelId="x", newModelId="y",
     items=["a", "a", "b"])
case("set_unicode_ids", "set", oldModelId="модель", newModelId="\u043c\u043e\u0434\u0435\u043b\u044c2",
     items=["модель", "b"])
case("set_separators_are_escaped", "set", oldModelId="a=b|c;d", newModelId="x=y",
     items=["a=b|c;d", "e"])

# ---- String?.replaceModelReference -----------------------------------------------------------
case("string_null_stays_null", "string", value=None, oldModelId="a", newModelId="b")
case("string_match_replaces", "string", value="a", oldModelId="a", newModelId="b")
case("string_match_to_null", "string", value="a", oldModelId="a", newModelId=None)
case("string_no_match_is_unchanged", "string", value="b", oldModelId="a", newModelId="c")
case("string_old_equals_new", "string", value="a", oldModelId="a", newModelId="a")
case("string_empty_matches_empty", "string", value="", oldModelId="", newModelId="x")
case("string_empty_value_other_old", "string", value="", oldModelId="a", newModelId="x")
case("string_separators_are_escaped", "string", value="a|b;c", oldModelId="a|b;c",
     newModelId="d|e;f")

# ---- Map<String, String>.replaceCustomModelAlias ---------------------------------------------
case("map_renames_to_absent_key_appends", "map", oldModelId="a", newModelId="b", alias="x",
     entries=[["a", "1"]])
case("map_replaces_value_in_place", "map", oldModelId="a", newModelId="c", alias="x",
     entries=[["a", "1"], ["b", "2"]])
case("map_removes_when_new_is_null", "map", oldModelId="a", newModelId=None, alias="x",
     entries=[["a", "1"], ["b", "2"]])
case("map_old_equals_new_moves_to_end", "map", oldModelId="a", newModelId="a", alias="3",
     entries=[["a", "1"], ["b", "2"]])
case("map_existing_target_value_overwritten_in_place", "map", oldModelId="a", newModelId="b",
     alias="3", entries=[["a", "1"], ["b", "2"]])
case("map_existing_target_before_old", "map", oldModelId="a", newModelId="b", alias="3",
     entries=[["b", "2"], ["a", "1"]])
case("map_blank_alias_removes_only", "map", oldModelId="a", newModelId="b", alias="",
     entries=[["a", "1"]])
case("map_space_alias_is_blank", "map", oldModelId="a", newModelId="b", alias="   ",
     entries=[["a", "1"]])
case("map_nbsp_alias_is_blank", "map", oldModelId="a", newModelId="b", alias=NBSP,
     entries=[["a", "1"]])
case("map_zwsp_alias_is_content", "map", oldModelId="a", newModelId="b",
     alias=ZWSP, entries=[["a", "1"]])
case("map_nbsp_padded_alias_is_trimmed", "map", oldModelId="a", newModelId="b",
     alias=NBSP + "x" + NBSP, entries=[["a", "1"]])
case("map_zwsp_padded_alias_is_kept", "map", oldModelId="a", newModelId="b",
     alias=ZWSP + "x" + ZWSP, entries=[["a", "1"]])
case("map_alias_is_trimmed", "map", oldModelId="a", newModelId="b", alias="  x  ",
     entries=[["a", "1"]])
case("map_inner_spaces_are_kept", "map", oldModelId="a", newModelId="b", alias="  x  y  ",
     entries=[["a", "1"]])
case("map_absent_old_still_adds_alias", "map", oldModelId="x", newModelId="b", alias="y",
     entries=[["a", "1"]])
case("map_absent_old_with_null_new_is_unchanged", "map", oldModelId="x", newModelId=None,
     alias="y", entries=[["a", "1"]])
case("map_empty_input_adds_alias", "map", oldModelId="a", newModelId="b", alias="x", entries=[])
case("map_old_equals_new_blank_alias_removes", "map", oldModelId="a", newModelId="a", alias="",
     entries=[["a", "1"], ["b", "2"]])
case("map_newline_alias_is_escaped", "map", oldModelId="a", newModelId="b", alias="x\ny",
     entries=[["a", "1"]])
case("map_unicode_ids", "map", oldModelId="модель", newModelId="новая", alias="псевдоним",
     entries=[["модель", "1"], ["b", "2"]])

os.makedirs(SCENARIOS_DIR, exist_ok=True)
for stale in os.listdir(SCENARIOS_DIR):
    if stale.endswith(".json"):
        os.remove(os.path.join(SCENARIOS_DIR, stale))
for entry in ALL:
    path = os.path.join(SCENARIOS_DIR, entry["name"] + ".json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(entry, handle, ensure_ascii=False)
        handle.write("\n")
print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")
