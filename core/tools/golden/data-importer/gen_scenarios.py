#!/usr/bin/env python3
"""Deterministic scenario generator for data/DataImporter.kt.

Every scenario carries a complete 97-key SettingsManager snapshot (the double refuses missing
keys), optional seeded memory/skill catalogs and database rows, and an ordered step list. Each
preview/import step names an ordered archive entry list (text or base64); the driver builds a real
ZIP and opens it through NativeBackupArchive's descriptor path.
"""
from __future__ import annotations

import base64
import copy
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

STABLE_A = "custom-provider-11111111-1111-4111-8111-111111111111"

BASE = {
    "selectedModel": "gpt-5.4",
    "contextCompactPrompt": "Compact the conversation",
    "thinkingLevel": "high",
    "openAiServiceTier": "default",
    "anthropicCacheTtl": "1h",
    "titleGenerationPrompt": "Summarize the chat",
    "modelSearchMethod": "semantic",
    "manualSearchMethod": "keyword",
    "appLanguage": "system",
    "webSearchProvider": "duckduckgo",
    "webSearchBaseUrl": "",
    "imageGenSize": "1024x1024",
    "imageTranscriptionPrompt": "Describe the image",
    "themeMode": "FOLLOW_DEVICE",
    "colorScheme": "dynamic_green",
    "schemeStyle": "tonal",
    "fontPreference": "app_default",
    "proxyType": "http",
    "proxyHost": "127.0.0.1",
    "proxyPort": "8080",
    "proxyUsername": "",
    "proxyBypass": "",
    "toolCallDisplayMode": "compact",
    "thinkingSegmentDisplayMode": "auto",
    "activeEmbeddingModelId": "",
    "customFontName": "",
    "customFontPath": "",
    "contextCompactModel": None,
    "titleGenerationModel": None,
    "imageGenModel": None,
    "imageTranscriptionModel": None,
    "activeSystemPromptId": None,
    "accessActiveMemory": True,
    "accessPastConversations": True,
    "accessSavedMemories": False,
    "accessSkills": True,
    "amoledEnabled": False,
    "anthropicCacheEnabled": True,
    "askUserEnabled": True,
    "autoCacheEnabled": True,
    "autoExpandActiveGroup": False,
    "automationToolsEnabled": False,
    "automationWakeLockEnabled": False,
    "autoUpdateCheck": True,
    "autoWrapCodeBlocks": True,
    "blurEffectsEnabled": True,
    "codeExecutionEnabled": False,
    "contextCompactEnabled": True,
    "contextCompactPreserveSystemPrompt": True,
    "detailedTokenUsage": False,
    "dynamicColor": True,
    "exactExecutionEnabled": False,
    "googleSearchEnabled": False,
    "hapticsEnabled": True,
    "imageGenEnabled": False,
    "imageTranscriptionEnabled": False,
    "openAiResponsesApiEnabled": True,
    "openAiServiceTierEnabled": False,
    "parseInlineDollarMath": False,
    "proxyEnabled": False,
    "ragSearchEnabled": False,
    "reduceMotion": False,
    "shellConfirmEnabled": True,
    "shellEnabled": True,
    "showDocumentationFab": True,
    "showUncachedNotification": True,
    "stickToBottom": True,
    "thinkingBudgetEnabled": False,
    "thinkingEnabled": True,
    "titleGenerationEnabled": True,
    "titleGenerationNotificationsEnabled": False,
    "visualizeContextRollout": False,
    "webSearchEnabled": True,
    "contextCompactRetainCount": 4,
    "contextCompactThresholdPercent": 90,
    "imageTranscriptionBatchSize": 3,
    "maxContextWindow": 128000,
    "searchContextWindow": 8,
    "searchMatchLimit": 10,
    "thinkingBudgetTokens": 4096,
    "webSearchNumResults": 5,
    "defaultMaxTokens": None,
    "ragThreshold": 0.5,
    "defaultTemperature": 0.7,
    "defaultTopP": None,
    "defaultFrequencyPenalty": None,
    "defaultPresencePenalty": None,
    "customModels": ["m1", "m2"],
    "enabledModels": ["m1"],
    "imageTranscriptionEnabledModels": ["whisper-large"],
    "modelAliases": {"m1": "One"},
    "providerBaseUrls": {STABLE_A: "https://a.example/v1"},
    "modelProviderNames": {"m1": True},
    "embeddingModels": [
        {
            "id": "emb-remote-1",
            "name": "Remote E5",
            "type": "REMOTE",
            "remoteModelName": "intfloat/e5-large",
            "remoteBaseUrl": "https://embed.example/v1",
            "remoteApiKey": "emb-secret",
            "localFilePath": "",
            "batchSize": 8,
        },
        {
            "id": "emb-local-1",
            "name": "Local MiniLM",
            "type": "LOCAL",
            "localFilePath": "/models/minilm.onnx",
            "batchSize": 4,
        },
    ],
    "shellDevices": [
        {
            "id": "dev-1",
            "name": "home-conch",
            "description": "Living room",
            "type": "conch",
            "serverUrl": "https://conch.example",
            "apiKey": "shell-secret",
            "sshPassword": "",
            "sshHost": "",
            "sshPort": 22,
            "sshUser": "root",
            "sshHostKey": "",
            "conchPublicKey": "",
        }
    ],
    "mcpServers": [
        {
            "id": "mcp-1",
            "name": "filesystem",
            "enabled": True,
            "url": "https://mcp.example",
            "transport": "streamable_http",
            "headers": {"Authorization": "Bearer mcp-secret"},
            "disabledTools": ["danger"],
        }
    ],
    "customProviders": [
        {
            "name": "MyProvider",
            "protocol": "openai",
            "id": STABLE_A,
            "legacyNames": ["OldName"],
            "responsesApiEnabled": True,
            "anthropicCacheEnabled": True,
            "anthropicCacheTtl": "1h",
        }
    ],
}

INDEX = "conv/index.json"
TASKS = "conv/tasks.json"
MANIFEST = "manifest.json"
SETTINGS = "settings.json"
SECRETS = "api_keys.json"
PROMPTS = "system_prompts.json"
CONVERSATIONS = "conversations.json"
EXTRA = "extra_settings.json"
CUSTOM_FONT = "custom_font/font"


def base_settings(**overrides):
    settings = copy.deepcopy(BASE)
    settings.update(overrides)
    return settings


def q(value):
    return "'" + str(value).replace("'", "''") + "'"


def sql_value(value):
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, (int, float)):
        return str(value)
    return q(value)


def insert(table, values):
    columns = ", ".join(values)
    return (
        f"INSERT INTO {table} ({columns}) VALUES ("
        + ", ".join(sql_value(value) for value in values.values())
        + ")"
    )


def seed_conversation(conv_id, title="t", **extra):
    values = {
        "id": conv_id,
        "title": title,
        "lastUpdated": 1,
        "dataChangedAt": 1,
        "origin": "user",
        "graduated": 0,
        "draftText": "",
        "hasUnreadGeneration": 0,
        "isPinned": 0,
    }
    values.update(extra)
    return insert("conversations", values)


def seed_run(run_id, conv_id, **extra):
    values = {
        "id": run_id,
        "conversationId": conv_id,
        "parentRunId": None,
        "status": "COMPLETED",
        "activeSlot": None,
        "startedAt": 1,
        "lastCheckpointAt": 1,
        "stopRequestedAt": None,
        "endedAt": 2,
        "endReason": "MODEL_COMPLETED",
        "currentPass": 0,
        "legacyAmbiguous": 0,
    }
    values.update(extra)
    return insert("runs", values)


def seed_message(msg_id, conv_id, run_id=None, **extra):
    values = {
        "id": msg_id,
        "conversationId": conv_id,
        "parentId": None,
        "text": "seed",
        "images": "[]",
        "tokenCount": 0,
        "status": "SUCCESS",
        "participant": "MODEL",
        "timestamp": 1,
        "runId": run_id,
        "runSequence": -1,
        "consumedAtPass": None,
        "sourceJson": None,
        "toolCallJson": None,
        "attachmentMeta": None,
        "modelName": None,
    }
    values.update(extra)
    return insert("messages", values)


def seed_task(task_id, **extra):
    values = {
        "id": task_id,
        "name": "n",
        "prompt": "p",
        "systemPrompt": None,
        "systemPromptId": None,
        "modelId": None,
        "cronExpr": "0 9 * * *",
        "runAt": None,
        "nextRunAt": 7,
        "enabled": 1,
        "createdAt": 1,
        "lastRunAt": None,
    }
    values.update(extra)
    return insert("tasks", values)


def seed_embedding(row_id, message_id, model_id="m-x"):
    return (
        "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
        "sourceFingerprint) VALUES ("
        + ", ".join([
            sql_value(row_id), sql_value(message_id), sql_value(model_id),
            "x'00'", "'chunk'", "2", "'fp'",
        ])
        + ")"
    )


def seed_ledger(model_id, state="CURRENT", source=3, completed=3, reconcile=0, updated=5):
    return insert(
        "semantic_index_ledger",
        {
            "modelId": model_id,
            "state": state,
            "sourceRevision": source,
            "completedRevision": completed,
            "reconcileRevision": reconcile,
            "updatedAt": updated,
        },
    )


def seed_pending_transfer(transfer_id, settings_json, mode="MERGE"):
    return insert(
        "conversation_settings_import_transfer",
        {
            "id": 1,
            "transferId": transfer_id,
            "settingsJson": settings_json,
            "mode": mode,
        },
    )


def manifest(version=6, categories=None, has_api_keys=False, app_version="9.9.9"):
    return json.dumps(
        {
            "agora_export_version": version,
            "app_version": app_version,
            "exported_at": "2026-01-02T03:04:05Z",
            "categories": categories or ["conversations", "memories", "system_prompts",
                                          "settings", "api_keys"],
            "has_api_keys": has_api_keys,
        },
        separators=(",", ":"),
    )


def conversation_entry(conversation_id):
    return "conv/items/" + conversation_id.encode("utf-8").hex() + ".json"


def index(*conversations):
    items = []
    for item in conversations:
        entry = dict(item)
        entry.setdefault("dataChangedAt", 1)
        items.append(entry)
    return json.dumps({"conversations": items}, separators=(",", ":"), ensure_ascii=False)


def graph_item(payload):
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


def gconv(conv_id, title="t", **extra):
    value = {"id": conv_id, "title": title, "lastUpdated": 1}
    value.update(extra)
    return value


def grun(run_id, conv_id, started=1, **extra):
    value = {
        "id": run_id,
        "conversationId": conv_id,
        "status": "COMPLETED",
        "startedAt": started,
        "lastCheckpointAt": started,
        "endedAt": started + 1,
        "endReason": "MODEL_COMPLETED",
    }
    value.update(extra)
    return value


def gmsg(msg_id, conv_id, ts, participant="MODEL", status="SUCCESS", run_id=None,
         run_seq=None, parent=None, text="t", images=None, attachment_meta=None, **extra):
    value = {
        "id": msg_id,
        "conversationId": conv_id,
        "text": text,
        "timestamp": ts,
        "participant": participant,
        "status": status,
    }
    if run_id is not None:
        value["runId"] = run_id
    if run_seq is not None:
        value["runSequence"] = run_seq
    if parent is not None:
        value["parentId"] = parent
    if images is not None:
        value["images"] = images
    if attachment_meta is not None:
        value["attachmentMeta"] = attachment_meta
    value.update(extra)
    return value


def tasks_json(*tasks):
    return json.dumps({"tasks": list(tasks)}, separators=(",", ":"), ensure_ascii=False)


def prompt(prompt_id, title, content="", system_items=None, user_items=None,
           assistant_items=None):
    value = {"id": prompt_id, "title": title, "content": content}
    if system_items is not None:
        value["systemItems"] = system_items
    if user_items is not None:
        value["userItems"] = user_items
    if assistant_items is not None:
        value["assistantItems"] = assistant_items
    return value


def prompts_json(*prompts):
    return json.dumps(list(prompts), separators=(",", ":"), ensure_ascii=False)


def text_entry(name, text):
    return {"name": name, "text": text}


def binary_entry(name, data):
    return {"name": name, "base64": base64.b64encode(data).decode("ascii")}


def write(name, **scenario):
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def write_scenario(name, *, steps, **rest):
    scenario = {
        "appVersion": "9.9.9",
        "settings": base_settings(),
        "steps": steps,
    }
    scenario.update(rest)
    write(name, **scenario)


# ---- a tiny but valid TTF with a Windows (3,1) name table --------------------------------

def make_ttf(family="Imported Font", subfamily=None, mac=False):
    records = []
    strings = b""
    entries = [(1, family)]
    if subfamily is not None:
        entries.append((2, subfamily))
    for name_id, text in entries:
        if mac:
            encoded = text.encode("mac_roman")
            platform, encoding, language = 1, 0, 0
        else:
            encoded = text.encode("utf-16-be")
            platform, encoding, language = 3, 1, 0x0409
        records.append((platform, encoding, language, name_id, len(encoded), len(strings)))
        strings += encoded
    count = len(records)
    header = (0).to_bytes(2, "big") + count.to_bytes(2, "big") + (6 + 12 * count).to_bytes(2, "big")
    body = b"".join(
        platform.to_bytes(2, "big") + encoding.to_bytes(2, "big")
        + language.to_bytes(2, "big") + name_id.to_bytes(2, "big")
        + length.to_bytes(2, "big") + offset.to_bytes(2, "big")
        for platform, encoding, language, name_id, length, offset in records
    )
    name_table = header + body + strings
    num_tables = 1
    search_range = 16
    entry_selector = 0
    range_shift = 0
    offset = 12 + 16
    directory = (
        (0x00010000).to_bytes(4, "big")
        + num_tables.to_bytes(2, "big")
        + search_range.to_bytes(2, "big")
        + entry_selector.to_bytes(2, "big")
        + range_shift.to_bytes(2, "big")
        + b"name"
        + (0).to_bytes(4, "big")
        + offset.to_bytes(4, "big")
        + len(name_table).to_bytes(4, "big")
    )
    return directory + name_table


PNG_BYTES = b"\x89PNG\r\n\x1a\nstand-image"
MP4_BYTES = b"\x00\x00\x00\x18ftypmp42stand-video"


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith(".json"):
            os.remove(os.path.join(OUT, name))

    c1 = conversation_entry("c1")
    c2 = conversation_entry("c2")

    # 01 — v6 preview: counts from the index/tasks stream, memories by prefix, prompts decode.
    write_scenario(
        "01-preview-v6",
        steps=[{
            "op": "preview",
            "archive": [
                text_entry(MANIFEST, manifest(6, has_api_keys=True)),
                text_entry(INDEX, index({"id": "c1", "entry": c1}, {"id": "c2", "entry": c2})),
                text_entry(c1, graph_item({"conversations": [gconv("c1")]})),
                text_entry(c2, graph_item({"conversations": [gconv("c2")]})),
                text_entry(TASKS, tasks_json(
                    {"id": "t1", "name": "n", "prompt": "p", "cronExpr": "0 9 * * *", "createdAt": 1},
                    {"id": "t2", "name": "n2", "prompt": "p2", "cronExpr": "0 9 * * *", "createdAt": 2},
                )),
                text_entry("memories/active_memory.md", "remember"),
                text_entry("memories/memory_db/a.md", "a"),
                text_entry(SETTINGS, "{}"),
                text_entry(SECRETS, "{}"),
                text_entry(PROMPTS, prompts_json(
                    prompt("p1", "Alpha", "a"),
                    prompt("p2", "Beta", "b"),
                )),
            ],
        }],
    )

    # 02 — legacy v4 preview: conversations.json is streamed once for both counts; a malformed
    # prompt list is swallowed to 0.
    legacy_conversations = json.dumps(
        {"conversations": [gconv("old")], "tasks": [
            {"id": "old-task", "name": "n", "prompt": "p", "cronExpr": "0 9 * * *", "createdAt": 1},
        ]},
        separators=(",", ":"),
    )
    write_scenario(
        "02-preview-legacy",
        steps=[{
            "op": "preview",
            "archive": [
                text_entry(MANIFEST, manifest(4)),
                text_entry(CONVERSATIONS, legacy_conversations),
                text_entry("memories/memory_db/a.md", "a"),
                text_entry(PROMPTS, "{not json"),
            ],
        }],
    )

    # 03 — preview failures: missing manifest, malformed manifest, unsupported version.
    write_scenario(
        "03-preview-invalid",
        steps=[
            {"op": "preview", "archive": [text_entry("other.json", "{}")]},
            {"op": "preview", "archive": [text_entry(MANIFEST, "{bad")]},
            {"op": "preview", "archive": [text_entry(MANIFEST, manifest(7))]},
        ],
    )

    # 04 — MERGE prompts: identical id+content is kept, same-id/different-content gets a new UUID,
    # duplicate titles walk the " (imported)" / " 2" suffix chain.
    existing_prompts = [
        prompt("p1", "Alpha", "a1"),
        prompt("p2", "Beta", "old"),
    ]
    imported_prompts = prompts_json(
        prompt("p1", "Alpha", "a1"),
        prompt("p2", "Beta", "new"),
        prompt("p3", "Beta", "new3"),
        prompt("p4", "Gamma", "g"),
    )
    write_scenario(
        "04-import-prompts-merge",
        systemPrompts=existing_prompts,
        steps=[{
            "op": "import",
            "decisions": {"SYSTEM_PROMPTS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["system_prompts"])),
                text_entry(PROMPTS, imported_prompts),
            ],
        }],
    )

    # 05 — REPLACE prompts: the imported list replaces the store; ids resolve identity.
    write_scenario(
        "05-import-prompts-replace",
        systemPrompts=existing_prompts,
        steps=[{
            "op": "import",
            "decisions": {"SYSTEM_PROMPTS": "REPLACE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["system_prompts"])),
                text_entry(PROMPTS, prompts_json(
                    prompt("r1", "Replaced", "r1"),
                    prompt("r2", "Second", "r2", system_items=[
                        {"id": "i1", "type": "CUSTOM", "value": "sys"},
                        {"id": "i2", "type": "PREDEFINED", "value": "time"},
                    ]),
                )),
            ],
        }],
    )

    # 06 — graph MERGE with media + conversation settings: media is copied (UUID names), the
    # graph writes, the settings transfer is completed by the coordinator, and the prompt resolver
    # maps the archived prompt id to the merged one.
    write_scenario(
        "06-import-graph-merge",
        systemPrompts=existing_prompts,
        seedSql=[
            seed_conversation("c1", title="OLD"),
            seed_run("r-old", "c1", startedAt=1),
        ],
        steps=[{
            "op": "import",
            "decisions": {"CONVERSATIONS": "MERGE", "SYSTEM_PROMPTS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["conversations", "system_prompts"])),
                text_entry(PROMPTS, prompts_json(
                    prompt("p1", "Alpha", "a1"),
                    prompt("p2", "Beta", "new"),
                )),
                text_entry(INDEX, index({"id": "c1", "entry": c1})),
                text_entry(c1, graph_item({
                    "conversations": [gconv(
                        "c1",
                        title="NEW",
                        systemPromptId="p2",
                        draftAttachments=json.dumps(
                            [{"localId": "a1", "uri": "media/images/img.png", "type": "image"}],
                            separators=(",", ":"),
                        ),
                        conversationSettings={"temperature": 0.7, "maxTokens": 512},
                    )],
                    "runs": [grun("r-new", "c1", started=10)],
                    "messages": [gmsg("m-new", "c1", 10, run_id="r-new", run_seq=0, text="hi",
                                      images=["media/images/img.png"])],
                    "loops": [{"conversationId": "c1", "intervalMs": 60000,
                               "prompt": "  loop  ", "cycleCount": -1, "active": True}],
                })),
                text_entry(TASKS, tasks_json(
                    {"id": "t1", "name": "  named  ", "prompt": "  body  ",
                     "systemPromptId": "p2", "cronExpr": " 0 9 * * * ", "nextRunAt": 9,
                     "enabled": True, "createdAt": 1},
                )),
                binary_entry("media/images/img.png", PNG_BYTES),
            ],
        }],
    )

    # 07 — REPLACE graph with a pending import transfer: completePendingImport applies MERGE
    # first, then the graph's REPLACE transfer is applied and the table is left empty.
    pending_json = json.dumps(
        {"c-pending": {"temperature": 0.3, "thinkingLevel": "low"}},
        separators=(",", ":"),
    )
    write_scenario(
        "07-import-graph-replace",
        seedSql=[
            seed_conversation("c1", title="OLD"),
            seed_run("r-old", "c1"),
            seed_message("m-old", "c1", run_id="r-old", text="old"),
            seed_pending_transfer("pending-1", pending_json, mode="MERGE"),
        ],
        steps=[{
            "op": "import",
            "decisions": {"CONVERSATIONS": "REPLACE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["conversations"])),
                text_entry(INDEX, index({"id": "c1", "entry": c1})),
                text_entry(c1, graph_item({
                    "conversations": [gconv("c1", title="NEW",
                                            conversationSettings={"maxTokens": 64})],
                    "runs": [],
                    "messages": [],
                    "loops": [],
                })),
                text_entry(TASKS, tasks_json()),
            ],
        }],
    )

    # 08 — a damaged conversation item fails staging: media was already copied and must be
    # deleted again; no database write or prompt save happens.
    write_scenario(
        "08-import-graph-failure",
        seedSql=[seed_conversation("keep", title="keep")],
        steps=[{
            "op": "import",
            "decisions": {"CONVERSATIONS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["conversations"])),
                text_entry(INDEX, index({"id": "c1", "entry": c1})),
                text_entry(c1, graph_item({
                    "conversations": [{"title": "no id"}],
                    "runs": [],
                    "messages": [],
                    "loops": [],
                })),
                text_entry(TASKS, tasks_json()),
                binary_entry("media/images/img.png", PNG_BYTES),
            ],
        }],
    )

    # 09 — direct FontUtils reads (Windows + Mac name tables, and a missing name table fallback).
    write_scenario(
        "09-font-utils",
        steps=[
            {"op": "font", "base64": base64.b64encode(
                make_ttf("Imported Font", "Bold")).decode("ascii")},
            {"op": "font", "base64": base64.b64encode(
                make_ttf("Caf\u00e9", mac=True)).decode("ascii")},
            {"op": "font", "base64": base64.b64encode(b"not a font").decode("ascii")},
        ],
    )

    # 10 — MERGE memories: existing active/meta/skill files are kept unless REPLACE;
    # createFile falls back to editFile on conflict.
    write_scenario(
        "10-import-memories-merge",
        memory={"active": "old active", "meta": "{}", "files": {"a.md": "old a"}},
        skills={"meta": "{}", "files": {"s.md": "old s"}},
        steps=[{
            "op": "import",
            "decisions": {"MEMORIES": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["memories"])),
                text_entry("memories/active_memory.md", "new active"),
                text_entry("memories/memory_db/memory_meta.json", '{"key":"val"}'),
                text_entry("memories/skill_db/skill_meta.json", '{"skill":"meta"}'),
                text_entry("memories/skill_db/s.md", "edited s"),
                text_entry("memories/skill_db/t.md", "new skill"),
                text_entry("memories/memory_db/b.md", "new memory"),
            ],
        }],
    )

    # 11 — REPLACE memories: everything deleted first, then imported; active cleared.
    write_scenario(
        "11-import-memories-replace",
        memory={"active": "old", "meta": "{}", "files": {"a.md": "old"}},
        skills={"meta": "{}", "files": {"s.md": "old"}},
        steps=[{
            "op": "import",
            "decisions": {"MEMORIES": "REPLACE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["memories"])),
                text_entry("memories/active_memory.md", "fresh active"),
                text_entry("memories/memory_db/memory_meta.json", '{"fresh":"meta"}'),
                text_entry("memories/skill_db/skill_meta.json", '{"skill":"fresh"}'),
                text_entry("memories/skill_db/s.md", "fresh skill"),
                text_entry("memories/memory_db/a.md", "fresh memory"),
            ],
        }],
    )

    # 12 — v6 SETTINGS import with custom font: restoreCustomFont reads TTF, saves path/name,
    # PortableSettingsArchive restores, reconcile runs invalidation/deletion on semantic ledger.
    write_scenario(
        "12-import-settings-v6",
        settings=base_settings(
            embeddingModels=[
                {"id": "emb-old-1", "name": "Old", "type": "REMOTE",
                 "remoteModelName": "a", "remoteBaseUrl": "https://a/v1", "remoteApiKey": "k", "batchSize": 8},
                {"id": "emb-old-2", "name": "Old2", "type": "REMOTE",
                 "remoteModelName": "b", "remoteBaseUrl": "https://b/v1", "remoteApiKey": "k", "batchSize": 8},
            ],
            activeEmbeddingModelId="emb-old-1",
        ),
        seedSql=[
            seed_ledger("emb-old-1", updated=5),
            seed_ledger("emb-old-2", updated=6),
            seed_ledger("emb-old-3", state="PENDING", source=7, completed=0, reconcile=0, updated=7),
            seed_embedding(1, "m1", "emb-old-1"),
            seed_embedding(2, "m2", "emb-old-2"),
        ],
        steps=[{
            "op": "import",
            "decisions": {"SETTINGS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["settings"])),
                text_entry(SETTINGS, json.dumps({
                    "selectedModel": "imported-model",
                    "embeddingModels": [
                        {"id": "emb-imported-1", "name": "Imported", "type": "REMOTE",
                         "remoteModelName": "new", "remoteBaseUrl": "https://new/v1", "batchSize": 8},
                        {"id": "emb-old-2", "name": "Old2", "type": "REMOTE",
                         "remoteModelName": "b-changed", "remoteBaseUrl": "https://b/v1", "batchSize": 8},
                    ],
                    "customFontPath": "/restored/font.ttf",
                    "customFontName": "Imported Font",
                }, separators=(",", ":"))),
                binary_entry(CUSTOM_FONT, make_ttf("Imported Font")),
            ],
        }],
    )

    # 13 — legacy v3 settings: allowLegacySecrets=true (version<5 and keys selected), extra_settings
    # restores conversation settings with allowed IDs, legacy font, and fontPreference reset.
    write_scenario(
        "13-import-settings-v3",
        settings=base_settings(
            fontPreference="custom",
            customFontPath="",
            customFontName="",
        ),
        seedSql=[
            seed_conversation("c-allow"),
        ],
        steps=[{
            "op": "import",
            "decisions": {"SETTINGS": "MERGE", "API_KEYS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(3, has_api_keys=True)),
                text_entry(SETTINGS, json.dumps({
                    "maxContextWindow": 32000,
                    "embeddingModels": [
                        {"id": "emb-legacy", "name": "Legacy", "type": "REMOTE",
                         "remoteModelName": "legacy", "remoteBaseUrl": "https://l/v1", "remoteApiKey": "legacy-key"},
                    ],
                }, separators=(",", ":"))),
                text_entry(SECRETS, json.dumps({"apiKeys": []}, separators=(",", ":"))),
                text_entry(EXTRA, json.dumps({
                    "conversationSettings": {
                        "c-allow": {"contextWindow": 16000, "temperature": 0.5},
                    }
                }, separators=(",", ":"))),
                binary_entry("custom_font/legacy.ttf", make_ttf("Legacy Font")),
            ],
        }],
    )

    # 14 — settings import with an error: malformed Anthropic cache TTL throws during restore;
    # the partially restored font file is deleted, reconcile still runs.
    write_scenario(
        "14-import-settings-error",
        steps=[{
            "op": "import",
            "decisions": {"SETTINGS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["settings"])),
                text_entry(SETTINGS, json.dumps({
                    "anthropicCacheTtl": "99m",
                }, separators=(",", ":"))),
                binary_entry(CUSTOM_FONT, make_ttf("Bad Font")),
            ],
        }],
    )

    # 15 — API keys import: secrets policy restores, warnings are added to errors.
    write_scenario(
        "15-import-keys",
        steps=[{
            "op": "import",
            "decisions": {"API_KEYS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["api_keys"])),
                text_entry(SECRETS, json.dumps({
                    "apiKeys": [{"id": "k1", "name": "A", "key": "secret", "provider": "google"}],
                    "activeApiKeyIds": {"google": "k1"},
                    "shellDevices": [{"id": "d1", "name": "D", "description": "", "type": "conch",
                                      "serverUrl": "https://x", "apiKey": "shell-key",
                                      "sshPassword": "", "sshHost": "", "sshPort": 22,
                                      "sshUser": "root", "sshHostKey": "", "conchPublicKey": ""}],
                    "embeddingModels": [{"id": "e1", "name": "E", "type": "REMOTE",
                                         "remoteModelName": "e", "remoteBaseUrl": "https://e/v1",
                                         "remoteApiKey": "emb-key"}],
                    "mcpServers": [{"id": "m1", "name": "M", "enabled": True,
                                    "url": "https://m", "transport": "streamable_http",
                                    "headers": {"Auth": "mcp-key"}, "disabledTools": []}],
                }, separators=(",", ":"))),
            ],
        }],
    )

    # 16 — full import all categories: progress sequence has 5 steps + final 1.0.
    c16 = conversation_entry("c1")
    write_scenario(
        "16-import-all",
        seedSql=[seed_conversation("c1")],
        steps=[{
            "op": "import",
            "decisions": {"CONVERSATIONS": "MERGE", "MEMORIES": "MERGE",
                          "SYSTEM_PROMPTS": "MERGE", "SETTINGS": "MERGE", "API_KEYS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["conversations","memories","system_prompts","settings","api_keys"], has_api_keys=True)),
                text_entry(PROMPTS, prompts_json(prompt("p1", "Full"))),
                text_entry(INDEX, index({"id": "c1", "entry": c16})),
                text_entry(c16, graph_item({"conversations": [gconv("c1")]})),
                text_entry(TASKS, tasks_json()),
                text_entry("memories/active_memory.md", "full"),
                text_entry(SETTINGS, json.dumps({"selectedModel": "full"}, separators=(",", ":"))),
                text_entry(SECRETS, json.dumps({"apiKeys": []}, separators=(",", ":"))),
                binary_entry("media/images/full.png", PNG_BYTES),
            ],
        }],
    )

    # 17 — various error conditions: missing manifest, unsupported version, missing settings, etc.
    write_scenario(
        "17-import-errors",
        steps=[
            {"op": "import", "archive": [text_entry("bad.zip", "{}")]},
            {"op": "import", "archive": [text_entry(MANIFEST, manifest(7))]},
            {"op": "import", "decisions": {"SETTINGS": "MERGE"},
             "archive": [text_entry(MANIFEST, manifest(6)), text_entry(PROMPTS, "[]")]},
            {"op": "import", "decisions": {"API_KEYS": "MERGE"},
             "archive": [text_entry(MANIFEST, manifest(6)), text_entry(PROMPTS, "[]")]},
        ],
    )

    # 18 — completePendingImport failure: failInitialLoad throws, error bubbles as "Conversations: settings unavailable".
    c18 = conversation_entry("c1")
    write_scenario(
        "18-import-pending-failure",
        seedSql=[seed_pending_transfer("pending-2", json.dumps({"c": {"maxTokens": 100}}, separators=(",", ":")))],
        steps=[{
            "op": "import",
            "decisions": {"CONVERSATIONS": "MERGE"},
            "failInitialLoad": "settings unavailable",
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["conversations"])),
                text_entry(INDEX, index({"id": "c1", "entry": c18})),
                text_entry(c18, graph_item({"conversations": [gconv("c1")]})),
                text_entry(TASKS, tasks_json()),
            ],
        }],
    )

    # 19 — completeImport failure: failImportApply throws after graph commit; transfer row remains.
    c19 = conversation_entry("c1")
    write_scenario(
        "19-import-complete-failure",
        seedSql=[seed_conversation("c1")],
        steps=[{
            "op": "import",
            "decisions": {"CONVERSATIONS": "MERGE"},
            "failImportApply": "apply failed",
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["conversations"])),
                text_entry(INDEX, index({"id": "c1", "entry": c19})),
                text_entry(c19, graph_item({"conversations": [gconv("c1")]})),
                text_entry(TASKS, tasks_json()),
            ],
        }],
    )

    # 20 — memories error: invalid JSON in memory_meta.json.
    write_scenario(
        "20-import-memory-error",
        steps=[{
            "op": "import",
            "decisions": {"MEMORIES": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["memories"])),
                text_entry("memories/memory_db/memory_meta.json", "{bad"),
            ],
        }],
    )

    # 21 — preview count errors: malformed index and tasks.
    write_scenario(
        "21-preview-count-errors",
        steps=[
            {"op": "preview", "archive": [
                text_entry(MANIFEST, manifest(6)),
                text_entry(INDEX, "{bad"),
                text_entry(TASKS, "{bad"),
            ]},
        ],
    )

    # 22 — no decisions: totalSteps=0 -> single onProgress(1.0), all counts zero.
    write_scenario(
        "22-import-no-decisions",
        steps=[{
            "op": "import",
            "decisions": {},
            "archive": [text_entry(MANIFEST, manifest(6, categories=["conversations"]))],
        }],
    )

    # 23 — font limit exceeded (declared size > 64MB via central directory patch).
    # We create the zip then patch the local/central header uncompressed size.
    write_scenario(
        "23-import-font-limit",
        steps=[{
            "op": "import",
            "decisions": {"SETTINGS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(6, categories=["settings"])),
                text_entry(SETTINGS, "{}"),
            ],
            "fontDeclaredSize": 70_000_000,
        }],
    )

    # 24 — v<4 font fallback: invalid TTF -> readFontName falls back to target name.
    write_scenario(
        "24-import-font-fallback",
        steps=[{
            "op": "import",
            "decisions": {"SETTINGS": "MERGE"},
            "archive": [
                text_entry(MANIFEST, manifest(3, categories=["settings"])),
                text_entry(SETTINGS, "{}"),
                binary_entry("custom_font/bad.ttf", b"not a font"),
            ],
        }],
    )


if __name__ == "__main__":
    main()
