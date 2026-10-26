#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the sandbox-path-resolver stand."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")
scenarios = []


def add(name, scenario):
    scenarios.append((name, scenario))


def resolve(name, path, home_mount_path, shared_storage_mount_path):
    add(name, {
        "op": "resolve",
        "path": path,
        "home_mount_path": home_mount_path,
        "shared_storage_mount_path": shared_storage_mount_path
    })


def walk(name, files, physical_root, virtual_root, remaining):
    add(name, {
        "op": "walk",
        "files": files,
        "physical_root": physical_root,
        "virtual_root": virtual_root,
        "remaining": remaining
    })


def file_read(name, path, offset, limit, total_bytes, content, lines, total_lines, truncated):
    add(name, {
        "op": "file_read",
        "path": path,
        "offset": offset,
        "limit": limit,
        "total_bytes": total_bytes,
        "content": content,
        "lines": lines,
        "total_lines": total_lines,
        "truncated": truncated
    })


# ---- resolveSandboxPath -----------------------------------------------------------------------
# Using standard mount paths: /mnt/home and /mnt/shared
HOME_MOUNT = "/mnt/home"
SHARED_MOUNT = "/mnt/shared"

resolve("resolve-rootfs-simple", "foo.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-rootfs-nested", "dir/bar.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-rootfs-absolute", "/baz.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-home-mount", "/mnt/home/foo.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-home-nested", "/mnt/home/dir/bar.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-shared-mount", "/mnt/shared/foo.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-shared-nested", "/mnt/shared/dir/bar.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-traversal-rootfs", "../../../etc/passwd", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-traversal-home", "/mnt/home/../../../etc/passwd", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-traversal-shared", "/mnt/shared/../../../etc/passwd", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-normalize-backslashes", "foo\\bar.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-normalize-repeated-slashes", "/foo//bar///baz.txt", HOME_MOUNT, SHARED_MOUNT)
resolve("resolve-normalize-dot-segments", "foo/./bar/../baz.txt", HOME_MOUNT, SHARED_MOUNT)

# ---- walkVirtualFiles ---------------------------------------------------------------------------
# Test files structure:
# /root/a.txt
# /root/b.md
# /root/dir/c.txt
# /root/dir/sub/d.txt
# /root/other/e.txt
WALK_FILES = [
    "/a.txt",
    "/b.md",
    "/dir/c.txt",
    "/dir/sub/d.txt",
    "/other/e.txt",
]

walk("walk-root-unlimited", WALK_FILES, "/root", "/", -1)
walk("walk-root-depth1", WALK_FILES, "/root", "/", 1)
walk("walk-root-depth2", WALK_FILES, "/root", "/", 2)
walk("walk-home-unlimited", [f.replace("/", "/mnt/home/") for f in WALK_FILES], "/root/home", "/mnt/home", -1)
walk("walk-shared-unlimited", [f.replace("/", "/mnt/shared/") for f in WALK_FILES], "/root/shared", "/mnt/shared", -1)

# ---- file_read ----------------------------------------------------------------------------------
# These scenarios describe expected outcomes for file_read with specific file content
# The actual file content and metadata are set up by the Kotlin trace dump
file_read("file-read-simple", "/test.txt", 0, 0, 11, "Hello World", 1, 1, False)
file_read("file-read-offset", "/test.txt", 6, 0, 11, "World", 1, 1, False)
file_read("file-read-limit", "/test.txt", 0, 5, 11, "Hello", 1, 1, False)
file_read("file-read-truncated", "/large.txt", 0, 100, 10000, "A" * 100, 1, 1000, True)


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.endswith(".json"):
            os.unlink(os.path.join(OUT, old))
    for index, (name, scenario) in enumerate(scenarios, start=1):
        path = os.path.join(OUT, f"{index:03d}-{name}.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(scenario, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(f"wrote {len(scenarios)} scenarios to {OUT}")


if __name__ == "__main__":
    main()