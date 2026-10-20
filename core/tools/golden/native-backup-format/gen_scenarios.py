#!/usr/bin/env python3
"""Generates the NativeBackupFormat scenarios."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def shell(api_key="", ssh_password=""):
    return {
        "id": "s1",
        "name": "dev",
        "description": "d",
        "type": "ssh",
        "serverUrl": "https://conch.example",
        "apiKey": api_key,
        "conchPublicKey": "pub",
        "sshHost": "host",
        "sshPort": 2222,
        "sshUser": "root",
        "sshPassword": ssh_password,
        "sshHostKey": "key",
    }


def mcp(headers):
    return {"id": "m1", "name": "server", "headers": headers}


def embedding(kind, remote_key="", local_path=""):
    return {
        "id": "e1",
        "name": "model",
        "type": kind,
        "remoteModelName": "text-embedding",
        "remoteBaseUrl": "https://api.example",
        "remoteApiKey": remote_key,
        "localFilePath": local_path,
        "batchSize": 16,
    }


CASES = [
    {"name": "format_constants", "op": "format"},
    {
        "name": "conversation_entries",
        "op": "entry",
        "ids": ["plain", "conv 1", "a/b", "Привет", "😀", "tab\tname", ""],
    },
    {
        "name": "models_remote",
        "op": "models",
        "settings": {
            "shell": shell("secret-key", "secret-pw"),
            "mcp": mcp({"Authorization": "Bearer x", "X-Empty": ""}),
            "embedding": embedding("REMOTE", "embed-key", "/tmp/model.bin"),
        },
    },
    {
        "name": "models_local",
        "op": "models",
        "settings": {
            "shell": shell(),
            "mcp": mcp({}),
            "embedding": embedding("LOCAL", "ignored-key", "/tmp/local.bin"),
        },
    },
    {
        "name": "models_remote_blank",
        "op": "models",
        "settings": {
            "shell": shell("", "pw"),
            "mcp": mcp({"A": "B"}),
            "embedding": embedding("REMOTE"),
        },
    },
]

os.makedirs(OUT, exist_ok=True)
for case in CASES:
    with open(os.path.join(OUT, case["name"] + ".json"), "w", encoding="utf-8") as stream:
        json.dump(case, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
print(f"wrote {len(CASES)} scenarios")
