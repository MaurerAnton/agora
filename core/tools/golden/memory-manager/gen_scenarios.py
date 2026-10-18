#!/usr/bin/env python3
"""Generates the scenario corpus for the MemoryManager golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.

Field conventions:
  name          scenario identifier; must equal the file stem
  mode          active|description_get|description_set|list|meta|meta_save|read|create|edit|delete|revision
  file_name     the memory file the scenario operates on
  contentParam  the create/edit payload (replaces content for create; new content for edit)
  setup_content pre-creates the file before the operation runs (for scenarios needing an existing file)
  old_string/new_string/new_name/description/meta_json/mode_param  mode-specific fields
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


# ---- active memory (operates on active_memory.md, not a catalog file) --------------------------
add("active_replace", "active", content_param="Hello world")
add("active_append", "active", active_seed="First line", content_param="Second line", mode_param="append")
add("active_prepend", "active", active_seed="Last line", content_param="First line", mode_param="prepend")
add("active_patch", "active", active_seed="Hello world", mode_param="patch",
    old_string="world", new_string="universe")
add("active_patch_not_found", "active", active_seed="Hello world", mode_param="patch",
    old_string="missing", new_string="found")
add("active_patch_multiple", "active", active_seed="foo bar foo", mode_param="patch",
    old_string="foo", new_string="baz")
add("active_patch_no_old_string", "active", active_seed="Hello world", mode_param="patch")

# ---- description get/set ------------------------------------------------------------------------
add("description_get_missing", "description_get", file_name="missing.md")
add("description_set", "description_set", file_name="test.md",
    setup_content="Original", description="A test file")
add("description_set_blank_removes", "description_set", file_name="test.md",
    setup_content="Original", description="")

# ---- list files ---------------------------------------------------------------------------------
add("list_empty", "list")
add("list_sorted", "list", file_name="b.md", setup_content="b")

# ---- meta json ----------------------------------------------------------------------------------
add("meta_empty", "meta")
add("meta_save_restore", "meta_save", meta_json='{"file1.md": "desc1", "file2.md": "desc2"}')

# ---- file read ----------------------------------------------------------------------------------
add("read_missing", "read", file_name="missing.md")
add("read_existing", "read", file_name="test.md", setup_content="Hello world")

# ---- file create --------------------------------------------------------------------------------
add("create_basic", "create", file_name="new.md", content_param="# Hello")
add("create_with_description", "create", file_name="test.md",
    content_param="Content", description="A test file")
add("create_existing", "create", file_name="existing.md",
    setup_content="Original", content_param="Duplicate")

# ---- file edit ----------------------------------------------------------------------------------
add("edit_replace", "edit", file_name="test.md", setup_content="Original", content_param="New content")
add("edit_rename", "edit", file_name="old.md", setup_content="Original", new_name="renamed.md")
add("edit_rename_and_content", "edit", file_name="old.md", setup_content="Original",
    content_param="New content", new_name="renamed.md")
add("edit_description", "edit", file_name="test.md", setup_content="Original",
    description="New description")
add("edit_patch", "edit", file_name="test.md", setup_content="Hello old world",
    old_string="old", new_string="new")
add("edit_patch_not_found", "edit", file_name="test.md", setup_content="Hello world",
    old_string="missing", new_string="found")
add("edit_patch_multiple", "edit", file_name="test.md", setup_content="foo bar foo",
    old_string="foo", new_string="bar")
add("edit_patch_no_match", "edit", file_name="test.md", setup_content="Hello world",
    old_string="", new_string="new")
add("edit_rename_conflict", "edit", file_name="test.md", setup_content="Original",
    new_name="existing.md", setup_target_name="existing.md", setup_target_content="Target")
add("edit_no_changes", "edit", file_name="test.md", setup_content="Original")

# ---- file delete --------------------------------------------------------------------------------
add("delete_existing", "delete", file_name="test.md", setup_content="Content to delete")
add("delete_missing", "delete", file_name="missing.md")

# ---- revision -----------------------------------------------------------------------------------
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
