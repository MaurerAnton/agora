#!/usr/bin/env python3
"""Generates the NativeBackupSecretsPolicy scenarios. Deterministic; rerun after editing."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def api(identifier, name, key, provider):
    return {"id": identifier, "name": name, "key": key, "provider": provider}


def shell(identifier, name, api_key="", ssh_password=""):
    return {
        "id": identifier,
        "name": name,
        "apiKey": api_key,
        "sshPassword": ssh_password,
    }


def embedding(identifier, name, kind, remote_key="", local_path="", batch_size=8):
    return {
        "id": identifier,
        "name": name,
        "type": kind,
        "remoteApiKey": remote_key,
        "localFilePath": local_path,
        "batchSize": batch_size,
    }


def mcp(identifier, name, headers):
    return {"id": identifier, "name": name, "headers": headers}


def settings(
    api_keys=(),
    active=(),
    web=(),
    proxy="",
    shells=(),
    embeddings=(),
    mcp_servers=(),
):
    return {
        "apiKeys": list(api_keys),
        "activeApiKeyIds": dict(active),
        "webSearchApiKeys": dict(web),
        "proxyPassword": proxy,
        "shellDevices": list(shells),
        "embeddingModels": list(embeddings),
        "mcpServers": list(mcp_servers),
    }


def secrets(
    api_keys=(),
    active=(),
    web=(),
    proxy="",
    shells=(),
    shell_api_keys=(),
    embedding_keys=(),
    mcp_headers=(),
):
    return {
        "apiKeys": list(api_keys),
        "activeApiKeyIds": dict(active),
        "webSearchApiKeys": dict(web),
        "proxyPassword": proxy,
        "shellDevices": {key: dict(value) for key, value in shells},
        "shellApiKeys": dict(shell_api_keys),
        "embeddingApiKeys": dict(embedding_keys),
        "mcpHeaders": {key: dict(value) for key, value in mcp_headers},
    }


def case(name, op, seed, backup=None, replace=None):
    scenario = {"name": name, "op": op, "settings": seed}
    if backup is not None:
        scenario["secrets"] = backup
    if replace is not None:
        scenario["replace"] = replace
    ALL.append(scenario)


# --- capture ---------------------------------------------------------------------------------

case(
    "capture_filters",
    "capture",
    settings(
        api_keys=[
            api("k1", "One", "sk-1", "openai"),
            api("k2", "Two", "sk-2", "google"),
        ],
        active={"openai": "k1"},
        web={"brave": "b1"},
        proxy="proxy-pw",
        shells=[
            shell("s1", "dev-one", "ak1", "sp1"),
            shell("blank", "blank"),
            shell("ssh-only", "ssh-only", "", "sp2"),
            shell("s1", "dev-duplicate", "ak2", "sp3"),
            shell("spaces", "spaces", "   ", "  "),
        ],
        embeddings=[
            embedding("e1", "remote", "REMOTE", "r1"),
            embedding("e2", "local", "LOCAL", "local-key", "/tmp/e2"),
            embedding("e3", "remote-blank", "REMOTE", ""),
            embedding("e4", "remote-spaces", "REMOTE", "   "),
        ],
        mcp_servers=[
            mcp("m1", "with-headers", {"Authorization": "Bearer x", "X-Empty": ""}),
            mcp("m2", "empty", {}),
        ],
    ),
)

case("capture_empty", "capture", settings())

case(
    "capture_whitespace",
    "capture",
    settings(
        proxy="  ",
        shells=[shell("s1", "spaces", "  ", "\t")],
        embeddings=[embedding("e1", "spaces", "REMOTE", "  ")],
        mcp_servers=[mcp("m1", "headers", {"A": " ", "B": ""})],
    ),
)

# --- replace ---------------------------------------------------------------------------------

case(
    "restore_replace_all",
    "restore",
    settings(
        api_keys=[api("old", "Old", "sk-old", "google")],
        active={"google": "old"},
        web={"ddg": "old"},
        proxy="old",
        shells=[shell("s1", "dev", "old-ak", "old-pw"), shell("s2", "unmatched", "u-ak", "u-pw")],
        embeddings=[
            embedding("e1", "remote", "REMOTE", "old-key"),
            embedding("e2", "local", "LOCAL", "local-old", "/tmp/e2"),
        ],
        mcp_servers=[mcp("m1", "server", {"Old": "1"})],
    ),
    secrets(
        api_keys=[api("k1", "Imported", "sk-new", "openai")],
        active={"openai": "k1", "broken": "missing"},
        web={"brave": "new"},
        proxy="new",
        shells=[
            ("s1", {"apiKey": "ak", "sshPassword": "pw"}),
            ("ghost", {"apiKey": "g", "sshPassword": ""}),
        ],
        shell_api_keys=[("dev", "legacy-ignored"), ("ghostname", "x")],
        embedding_keys=[("e1", "newkey"), ("e2", "local-ignore"), ("orphan", "x")],
        mcp_headers=[("m1", {"A": "B"}), ("orphan", {"C": "D"})],
    ),
    replace=True,
)

case(
    "restore_replace_clears",
    "restore",
    settings(
        api_keys=[api("old", "Old", "sk-old", "google")],
        active={"google": "old"},
        web={"ddg": "old"},
        proxy="old",
        shells=[shell("s1", "dev", "old-ak", "old-pw")],
        embeddings=[
            embedding("e1", "remote", "REMOTE", "old-key"),
            embedding("e2", "local", "LOCAL", "local-old", "/tmp/e2"),
        ],
        mcp_servers=[mcp("m1", "server", {"Old": "1"})],
    ),
    secrets(),
    replace=True,
)

case(
    "restore_replace_legacy_shell",
    "restore",
    settings(
        shells=[
            shell("s2", "dev", "old-ak", "old-pw"),
            shell("s3", "other", "other-ak", "other-pw"),
        ],
    ),
    secrets(shell_api_keys=[("dev", "legacy")]),
    replace=True,
)

# --- merge -----------------------------------------------------------------------------------

case(
    "restore_merge_semantics",
    "restore",
    settings(
        api_keys=[api("a", "A", "ka", "google")],
        active={"old": "a"},
        web={"brave": "old", "keep": "k"},
        proxy="existing",
        shells=[shell("s1", "dev", "old-ak", "old-pw"), shell("s2", "other", "o-ak", "o-pw")],
        embeddings=[
            embedding("e1", "remote", "REMOTE", "old-key"),
            embedding("e2", "local", "LOCAL", "local-old", "/tmp/e2"),
        ],
        mcp_servers=[mcp("m1", "server", {"Old": "1"})],
    ),
    secrets(
        api_keys=[
            api("b", "B", "kb", "openai"),
            api("c", "C", "ka", "google"),
            api("d", "D", "ka", "anthropic"),
            api("a", "A2", "ka2", "google"),
        ],
        active={"x": "a", "y": "b", "z": "c", "q": "missing"},
        web={"brave": "new", "ddg": "d"},
        proxy="",
        shells=[
            ("s1", {"apiKey": "newak", "sshPassword": "newpw"}),
            ("ghost", {"apiKey": "g", "sshPassword": "gp"}),
        ],
        shell_api_keys=[("dev", "legacy"), ("ghostname", "x")],
        embedding_keys=[("e1", "newkey"), ("e2", "local-ignore"), ("orphan", "x")],
        mcp_headers=[("m1", {"New": "2"}), ("orphan", {"C": "D"})],
    ),
    replace=False,
)

case(
    "restore_merge_legacy_shell",
    "restore",
    settings(
        shells=[shell("s1", "dev", "old-ak", "old-pw"), shell("s2", "other", "o-ak", "o-pw")],
    ),
    secrets(shell_api_keys=[("dev", "legacy")]),
    replace=False,
)

case(
    "restore_empty_merge",
    "restore",
    settings(
        api_keys=[api("a", "A", "ka", "google")],
        active={"old": "a"},
        web={"brave": "old"},
        proxy="existing",
        shells=[shell("s1", "dev", "old-ak", "old-pw")],
        embeddings=[embedding("e1", "remote", "REMOTE", "old-key")],
        mcp_servers=[mcp("m1", "server", {"Old": "1"})],
    ),
    secrets(),
    replace=False,
)


def main() -> int:
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for scenario in ALL:
        path = os.path.join(SCENARIOS_DIR, scenario["name"] + ".json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(scenario, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    print(f"wrote {len(ALL)} scenarios into {SCENARIOS_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
