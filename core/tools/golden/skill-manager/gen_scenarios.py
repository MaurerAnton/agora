#!/usr/bin/env python3
"""Generates the scenario corpus for the SkillManager golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.

Field conventions:
  name              scenario identifier; must equal the file stem
  mode              description_get|list|catalog|meta|meta_save|read|create|edit|delete|revision
  file_name         the skill file the scenario operates on (resolveFile applies the .md rule)
  content_param     the create/edit payload (blank maps to null, like the driver's takeIf)
  setup_content     pre-creates file_name through the real createFile before the operation
  setup_description description attached to the setup create (list/description seeding)
  setup_target_*    pre-creates a second file (rename targets/conflicts)
  setup_raw_name/content  writes a raw neighbour file into skill_db (extension filtering)
  setup_raw_meta    writes a raw skill_meta.json (corrupt-metadata path)
  old_string/new_string/new_name/description/meta_json  mode-specific fields
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def add(name, mode, **kw):
    out = {"name": name, "mode": mode}
    out.update(kw)
    ALL.append(out)


# ---- description lookup --------------------------------------------------------------------------
add("description_get_missing", "description_get", file_name="missing.md")
add("description_get_existing", "description_get", file_name="guide.md",
    setup_content="# Guide", setup_description="Guide for agents")

# ---- listing (.md filter, flat names, sorted by name) --------------------------------------------
add("list_empty", "list")
add("list_sorted", "list", file_name="beta", setup_content="beta",
    setup_target_name="alpha.md", setup_target_content="alpha")
add("list_filters_non_md", "list", file_name="keep.md", setup_content="keep",
    setup_raw_name="notes.txt", setup_raw_content="not a skill")
add("list_with_description", "list", file_name="guide", setup_content="# Guide",
    setup_description="Guide for agents")

# ---- catalog rendering ---------------------------------------------------------------------------
add("catalog_empty", "catalog")
add("catalog_with_files", "catalog", file_name="guide", setup_content="# Guide",
    setup_description="Guide for agents", setup_target_name="notes",
    setup_target_content="scratch")
add("catalog_description_newline", "catalog", file_name="guide", setup_content="# Guide",
    setup_description="line one\n  line two  ")

# ---- metadata json -------------------------------------------------------------------------------
add("meta_empty", "meta")
add("meta_save_restore", "meta_save",
    meta_json='{"guide.md": "Guide for agents", "notes.md": ""}')
add("meta_save_invalid", "meta_save", meta_json="not json")
add("meta_corrupt_list", "list", setup_raw_meta="{")

# ---- file read -----------------------------------------------------------------------------------
add("read_missing", "read", file_name="missing.md")
add("read_existing", "read", file_name="guide.md", setup_content="Hello world")
add("read_adds_md_extension", "read", file_name="guide", setup_content="Hello world")

# ---- file create ---------------------------------------------------------------------------------
add("create_basic", "create", file_name="new.md", content_param="# Hello")
add("create_adds_md_extension", "create", file_name="notes", content_param="# Notes")
add("create_with_description", "create", file_name="guide.md",
    content_param="Content", description="Guide for agents")
add("create_existing", "create", file_name="existing.md",
    setup_content="Original", content_param="Duplicate")
add("create_sanitizes_separators", "create", file_name="nested/thing",
    content_param="# Nested")

# ---- file edit -----------------------------------------------------------------------------------
add("edit_replace", "edit", file_name="guide.md", setup_content="Original",
    content_param="New content")
add("edit_rename", "edit", file_name="old.md", setup_content="Original",
    new_name="renamed.md")
add("edit_rename_and_content", "edit", file_name="old.md", setup_content="Original",
    content_param="New content", new_name="renamed.md")
add("edit_description", "edit", file_name="guide.md", setup_content="Original",
    description="New description")
add("edit_description_unchanged", "edit", file_name="guide.md", setup_content="Original",
    setup_description="Same description", description="Same description")
add("edit_patch", "edit", file_name="guide.md", setup_content="Hello old world",
    old_string="old", new_string="new")
add("edit_patch_not_found", "edit", file_name="guide.md", setup_content="Hello world",
    old_string="missing", new_string="found")
add("edit_patch_multiple", "edit", file_name="guide.md", setup_content="foo bar foo",
    old_string="foo", new_string="bar")
add("edit_patch_no_match", "edit", file_name="guide.md", setup_content="Hello world",
    old_string="", new_string="new")
add("edit_rename_conflict", "edit", file_name="guide.md", setup_content="Original",
    new_name="existing.md", setup_target_name="existing.md", setup_target_content="Target")
add("edit_no_changes", "edit", file_name="guide.md", setup_content="Original")

# ---- file delete ---------------------------------------------------------------------------------
add("delete_existing", "delete", file_name="guide.md",
    setup_content="Content to delete", setup_description="Guide for agents")
add("delete_missing", "delete", file_name="missing.md")

# ---- revision ------------------------------------------------------------------------------------
add("revision_check", "revision")


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
