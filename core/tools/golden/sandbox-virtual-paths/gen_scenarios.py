#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the sandbox-virtual-paths stand."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")
scenarios = []


def add(name, scenario):
    scenarios.append((name, scenario))


def norm(name, path):
    add(name, {"op": "normalize", "path": path})


def glob(name, files, pattern):
    add(name, {"op": "glob", "files": files, "pattern": pattern})


FILES = [
    "/a.txt",
    "/b.md",
    "/dir/a.txt",
    "/dir/b.md",
    "/dir/sub/c.txt",
    "/dir/sub/deep/d.md",
    "/other/e.txt",
]

# ---- normalizeVirtualPath -------------------------------------------------------------------
norm("norm-empty", "")
norm("norm-spaces", "   ")
norm("norm-tabs", "\t\n")
norm("norm-nbsp", "\u00a0")
norm("norm-zero-width-space", "\u200b")
norm("norm-relative", "foo")
norm("norm-absolute", "/foo")
norm("norm-relative-nested", "foo/bar")
norm("norm-backslashes", "foo\\bar")
norm("norm-unc", "\\\\server\\share")
norm("norm-repeated-slashes", "/foo//bar///")
norm("norm-root", "/")
norm("norm-all-slashes", "///")
norm("norm-trailing-slash", "/foo/")
norm("norm-relative-trailing-slash", "foo/")
norm("norm-padded", "  /foo/bar  ")
norm("norm-dot-segment", "a/../b")
norm("norm-dot", ".")
norm("norm-dotdot", "..")
norm("norm-tilde", "~")
norm("norm-drive", "C:\\foo\\bar")
norm("norm-unicode", "héllo/世界")
norm("norm-internal-space", "a b")
norm("norm-space-slash", " / ")
norm("norm-double-leading-trailing", "//foo//")
norm("norm-relative-double-trailing", "foo//")
norm("norm-backslash-only", "\\")
norm("norm-double-backslash", "a\\\\b")
norm("norm-dot-segment-inner", "a/./b")
norm("norm-two-slashes", "//")

# ---- globMatch ------------------------------------------------------------------------------
glob("glob-blank", FILES, "")
glob("glob-space", FILES, " ")
glob("glob-star-txt", FILES, "*.txt")
glob("glob-doublestar-txt", FILES, "**/*.txt")
glob("glob-dir-txt", FILES, "dir/*.txt")
glob("glob-absolute-dir-txt", FILES, "/dir/*.txt")
glob("glob-dir-doublestar-txt", FILES, "dir/**/*.txt")
glob("glob-doublestar-md", FILES, "**/*.md")
glob("glob-question-txt", FILES, "?.txt")
glob("glob-a-star-txt", FILES, "a*.txt")
glob("glob-class", FILES, "[ab].txt")
glob("glob-negated-class", FILES, "[!a].txt")
glob("glob-dir-class", FILES, "dir/[ab].txt")
glob("glob-trailing-slash", FILES, "dir/")
glob("glob-root", FILES, "/")
glob("glob-doublestar", FILES, "**")
glob("glob-star", FILES, "*")
glob("glob-backslash-txt", FILES, "\\*.txt")
glob("glob-dir-backslash-txt", FILES, "dir\\*.txt")
glob("glob-no-match", FILES, "nope/*.txt")
glob("glob-dir-sub-txt", FILES, "dir/sub/*.txt")
glob("glob-dir-doublestar", FILES, "dir/**")
glob("glob-deep-md", FILES, "**/deep/*.md")
glob("glob-sub-doublestar", FILES, "**/sub/**")
glob("glob-backslash-path", ["\\a\\b.txt"], "a/b.txt")
glob("glob-unicode", ["/héllo/世界.txt"], "*.txt")
glob("glob-empty-files", [], "*.txt")
glob("glob-padded-absolute", FILES, " /a.txt ")
glob("glob-padded-name", FILES, "a.txt ")
glob("glob-dir-star", FILES, "dir/*")
glob("glob-exact", FILES, "/dir/sub/deep/d.md")
glob("glob-exact-miss", FILES, "/dir/sub/deep/x.md")


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
