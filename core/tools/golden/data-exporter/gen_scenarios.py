#!/usr/bin/env python3
"""Deterministic scenario generator for DataExporter.

Every export scenario carries a complete 97-key settings snapshot (the SettingsManager double
refuses missing keys). Scenarios seed the schema-36 SQLite database, write memory/skill catalogs
and media fixtures, then run one or more export steps. Baseline steps are self-generated: a later
step may name an earlier step's output as its baseline file.
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


def conversation(conv_id, title="t", last=1, changed=1, **extra):
    values = {
        "id": conv_id,
        "title": title,
        "lastUpdated": last,
        "dataChangedAt": changed,
        "origin": "user",
        "graduated": 0,
        "draftText": "",
        "hasUnreadGeneration": 0,
        "isPinned": 0,
    }
    values.update(extra)
    return insert("conversations", values)


def run(run_id, conv_id, status="COMPLETED", started=1, checkpoint=0, **extra):
    values = {
        "id": run_id,
        "conversationId": conv_id,
        "parentRunId": None,
        "status": status,
        "activeSlot": None,
        "startedAt": started,
        "lastCheckpointAt": checkpoint,
        "stopRequestedAt": None,
        "endedAt": started + 1,
        "endReason": "MODEL_COMPLETED",
        "currentPass": 0,
        "legacyAmbiguous": 0,
    }
    values.update(extra)
    return insert("runs", values)


def message(msg_id, conv_id, run_id, text="hello", images=None, seq=0, **extra):
    values = {
        "id": msg_id,
        "conversationId": conv_id,
        "parentId": None,
        "text": text,
        "images": json.dumps(images or [], separators=(",", ":")),
        "tokenCount": 0,
        "status": "SUCCESS",
        "participant": "MODEL",
        "timestamp": 1,
        "runId": run_id,
        "runSequence": seq,
    }
    values.update(extra)
    return insert("messages", values)


def loop(conv_id, interval=60000, prompt=None, cycle=0, max_cycles=None, active=True):
    return insert(
        "loops",
        {
            "conversationId": conv_id,
            "intervalMs": interval,
            "prompt": prompt,
            "nextFireAt": 99,
            "cycleCount": cycle,
            "maxCycles": max_cycles,
            "active": active,
            "revision": 3,
        },
    )


def task(task_id="task-1", **extra):
    values = {
        "id": task_id,
        "name": "Daily",
        "prompt": "Do it",
        "systemPrompt": None,
        "systemPromptId": None,
        "modelId": None,
        "cronExpr": "0 9 * * *",
        "runAt": None,
        "nextRunAt": 0,
        "enabled": 0,
        "createdAt": 5,
        "lastRunAt": None,
    }
    values.update(extra)
    return insert("tasks", values)


def fixture(name, content, uri=None):
    entry = {
        "file": name,
        "base64": base64.b64encode(content.encode("utf-8")).decode("ascii"),
    }
    if uri is not None:
        entry["uri"] = uri
    return entry


def write(name, *, steps, settings=None, seed=None, media=None, memory=None,
          skills=None, font=None, conversation_settings=None, system_prompts=None,
          api_keys=None, app_version="9.9.9", **extra):
    scenario = {
        "appVersion": app_version,
        "settings": settings if settings is not None else base_settings(),
        "steps": steps,
    }
    if seed:
        scenario["seedSql"] = seed
    if media:
        scenario["media"] = media
    if memory is not None:
        scenario["memory"] = memory
    if skills is not None:
        scenario["skills"] = skills
    if font is not None:
        scenario["font"] = font
    if conversation_settings is not None:
        scenario["conversationSettings"] = conversation_settings
    if system_prompts is not None:
        scenario["systemPrompts"] = system_prompts
    if api_keys is not None:
        scenario["apiKeys"] = api_keys
    scenario.update(extra)
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def export(categories, *, include_api_keys=False, baseline_step=None, output=None,
           corrupt_baseline=False, stamp_baseline=False):
    step = {"op": "export", "categories": categories, "includeApiKeys": include_api_keys}
    if baseline_step is not None:
        step["baselineStep"] = baseline_step
    if output is not None:
        step["output"] = output
    if corrupt_baseline:
        step["corruptBaseline"] = True
    if stamp_baseline:
        step["stampBaseline"] = True
    return step


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith(".json"):
            os.remove(os.path.join(OUT, name))

    write("01-empty-categories", steps=[export([])])

    write(
        "02-conversations-defaults",
        seed=[
            conversation("c1", title="one", last=10, changed=11),
            run("r1", "c1"),
            message("m1", "c1", "r1", text="first", seq=0),
            loop("c1", prompt="Keep going", max_cycles=None),
            task(),
        ],
        steps=[export(["conversations"])],
    )

    write(
        "03-conversations-full",
        seed=[
            conversation(
                "c1",
                title="Full",
                last=100,
                changed=200,
                selectedBranchesJson='{"m1":"m2"}',
                systemPromptId="p1",
                modelId="model-1",
                taskId="task-1",
                origin="task",
                graduated=1,
                draftText="draft text",
                draftAttachments=json.dumps(
                    [
                        {
                            "localId": "a1",
                            "uri": "file://<media>/images/draft.png",
                            "type": "image",
                            "fileName": "draft.png",
                            "mimeType": "image/png",
                            "fileSize": 5,
                        }
                    ],
                    separators=(",", ":"),
                ),
                selectedRunBranchesJson='{"r1":"r2"}',
                isPinned=1,
            ),
            run(
                "r1",
                "c1",
                status="STOPPED",
                started=10,
                checkpoint=11,
                stopRequestedAt=12,
                endedAt=13,
                endReason="USER_STOPPED",
                currentPass=2,
                legacyAmbiguous=1,
            ),
            run(
                "r2",
                "c1",
                status="ACTIVE",
                started=14,
                checkpoint=15,
                parentRunId="r1",
                activeSlot=1,
                endedAt=None,
                endReason=None,
                currentPass=0,
            ),
            message(
                "m1",
                "c1",
                "r1",
                text="hello <world>",
                images=["file://<media>/images/a.png"],
                seq=7,
                parentId=None,
                thoughts="thinking",
                thoughtTitle="Title",
                tokenCount=9,
                inputTokenCount=1,
                cachedInputTokenCount=2,
                cacheWriteInputTokenCount=3,
                uncachedInputTokenCount=4,
                outputTokenCount=5,
                reasoningTokenCount=6,
                generationDurationMs=700,
                status="ERROR",
                participant="USER",
                timestamp=42,
                thoughtTimeMs=70,
                modelName="model-x",
                attachmentMeta=json.dumps(
                    {
                        "items": [
                            {
                                "originalUri": "file://<media>/images/a.png",
                                "type": "image",
                                "file_name": "a.png",
                                "image_index": 0,
                                "page_count": 1,
                            }
                        ]
                    },
                    separators=(",", ":"),
                ),
                runId="r1",
                runSequence=7,
                consumedAtPass=3,
                sourceJson='{"provider":"x"}',
            ),
            task(
                "task-1",
                name="Named",
                prompt="Prompt",
                systemPrompt="sys",
                systemPromptId="p1",
                modelId="model-1",
                cronExpr="0 9 * * *",
                runAt=123,
                createdAt=5,
                lastRunAt=99,
            ),
        ],
        media=[fixture("images/a.png", "A"), fixture("images/draft.png", "D")],
        conversation_settings={
            "c1": {
                "contextWindow": 32000,
                "temperature": 0.7,
                "maxTokens": 512,
                "topP": 0.9,
                "frequencyPenalty": 0.25,
                "presencePenalty": -0.5,
                "codeExecutionEnabled": True,
                "googleSearchEnabled": False,
                "openAiWebSearchEnabled": True,
                "thinkingEnabled": True,
                "thinkingLevel": "high",
                "thinkingBudgetEnabled": True,
                "thinkingBudgetTokens": 4096,
                "openAiServiceTierEnabled": False,
                "openAiServiceTier": "default",
                "webSearchEnabled": True,
                "shellEnabled": False,
                "lowContextModeEnabled": True,
            }
        },
        steps=[export(["conversations"])],
    )

    write(
        "04-media-rewrite",
        seed=[
            conversation("c1", title="media", changed=3),
            run("r1", "c1"),
            message(
                "m1",
                "c1",
                "r1",
                images=[
                    "file://<media>/images/shared.png",
                    "content://media/external/images/1",
                ],
                attachmentMeta=json.dumps(
                    {
                        "items": [
                            {
                                "originalUri": "file://<media>/images/shared.png",
                                "type": "image",
                                "image_index": 0,
                                "page_count": 1,
                            },
                            {
                                "originalUri": "file://<media>/videos/clip.mp4",
                                "type": "video",
                                "file_size": 4,
                            },
                            {
                                "originalUri": "file://<media>/images/gone.png",
                                "type": "image",
                                "image_index": 1,
                                "page_count": 1,
                            },
                            {
                                "originalUri": "file://<media>/images/out-of-range.png",
                                "type": "image",
                                "image_index": 5,
                                "page_count": 1,
                            },
                            {
                                "originalUri": "file://<media>/videos/missing.mp4",
                                "type": "video",
                                "file_size": 9,
                            },
                            {"type": "file", "text_content": "inline", "file_name": "n.txt"},
                        ]
                    },
                    separators=(",", ":"),
                ),
                toolCallJson=json.dumps(
                    [
                        {
                            "type": "tool_call",
                            "content": "",
                            "toolName": "read",
                            "toolImages": [
                                {
                                    "path": "file://<media>/images/tool.png",
                                    "mimeType": "image/png",
                                    "sizeBytes": 4,
                                    "sha256": "aa",
                                },
                                {
                                    "path": "file://<media>/images/tool-missing.png",
                                    "mimeType": "image/png",
                                    "sizeBytes": 4,
                                    "sha256": "bb",
                                },
                            ],
                        }
                    ],
                    separators=(",", ":"),
                ),
            ),
            message(
                "m2",
                "c1",
                "r1",
                images=["file://<media>/images/shared.png", "<media>/images/bare.png"],
                seq=1,
                attachmentMeta=None,
            ),
        ],
        media=[
            fixture("images/shared.png", "S"),
            fixture("images/bare.png", "B"),
            fixture("images/tool.png", "T"),
            fixture("videos/clip.mp4", "V"),
            fixture(
                "images/content-1.png",
                "C",
                uri="content://media/external/images/1",
            ),
        ],
        steps=[export(["conversations"])],
    )

    write(
        "05-baseline-reuse",
        seed=[
            conversation("c1", title="reuse", changed=5),
            conversation("c2", title="changed", changed=5),
            run("r1", "c1"),
            run("r2", "c2"),
            message("m1", "c1", "r1", images=["file://<media>/shared.png"], seq=0),
            message("m2", "c2", "r2", images=["file://<media>/shared.png"], seq=0),
        ],
        media=[fixture("shared.png", "SHARED")],
        steps=[
            export(["conversations"]),
            export(["conversations"], baseline_step=0, stamp_baseline=True),
        ],
    )
    write(
        "06-baseline-changed",
        seed=[
            conversation("c1", title="reuse", changed=5),
            conversation("c2", title="changed", changed=5),
            run("r1", "c1"),
            run("r2", "c2"),
            message("m1", "c1", "r1", images=["file://<media>/shared.png"], seq=0),
            message("m2", "c2", "r2", images=["file://<media>/shared.png"], seq=0),
        ],
        media=[fixture("shared.png", "SHARED")],
        steps=[
            export(["conversations"]),
            {
                **export(["conversations"], baseline_step=0, stamp_baseline=True),
                "preSql": ["UPDATE conversations SET dataChangedAt = 6 WHERE id = 'c2'"],
            },
        ],
    )

    write(
        "07-baseline-invalid",
        seed=[
            conversation("c1", title="reuse", changed=5),
            run("r1", "c1"),
            message("m1", "c1", "r1", images=["file://<media>/shared.png"], seq=0),
        ],
        media=[fixture("shared.png", "SHARED")],
        steps=[
            export(["conversations"]),
            export(["conversations"], baseline_step=0, corrupt_baseline=True),
        ],
    )

    write(
        "07b-baseline-default-revision",
        seed=[
            conversation("c1", title="reuse", changed=5),
            run("r1", "c1"),
            message("m1", "c1", "r1", images=["file://<media>/shared.png"], seq=0),
        ],
        media=[fixture("shared.png", "SHARED")],
        steps=[
            export(["conversations"]),
            export(["conversations"], baseline_step=0),
        ],
    )

    write(
        "08-memories-skills",
        memory={
            "active": "active memory\nline",
            "files": {
                "notes.md": "note body",
                "nested/deep.md": "deep body",
                "ignored.txt": "not a memory",
            },
            "meta": '{"notes.md":"Note description"}',
        },
        skills={
            "files": {
                "alpha.md": "alpha body",
                "beta.md": "beta body",
                "ignored.txt": "not a skill",
            },
            "meta": '{"alpha.md":"Alpha skill"}',
        },
        steps=[export(["memories"])],
    )

    write(
        "09-memories-empty",
        memory={"active": "", "files": {"a.md": "A"}, "meta": "{}"},
        skills={"files": {}, "meta": "{}"},
        steps=[export(["memories"])],
    )

    write(
        "10-system-prompts",
        system_prompts=[
            {
                "id": "p1",
                "title": "Plain",
                "content": "You are helpful",
                "systemItems": [],
                "userItems": [],
                "assistantItems": [],
                "userPrependItems": [],
                "userPostpendItems": [],
            },
            {
                "id": "p2",
                "title": "Templated",
                "content": "",
                "systemItems": [
                    {"id": "i1", "type": "CUSTOM", "value": "System "},
                    {"id": "i2", "type": "PREDEFINED", "value": "time"},
                ],
                "userItems": [{"id": "i3", "type": "PREDEFINED", "value": "prompt"}],
                "assistantItems": [{"id": "i4", "type": "CUSTOM", "value": "Assistant"}],
                "userPrependItems": [{"id": "i5", "type": "CUSTOM", "value": "Legacy"}],
                "userPostpendItems": [],
            },
        ],
        steps=[export(["system_prompts"])],
    )

    write(
        "11-settings-font",
        settings=base_settings(customFontName="My Font", customFontPath="<font>"),
        font={"name": "my.ttf", "base64": base64.b64encode(bytes([0, 1, 2, 255])).decode("ascii")},
        steps=[export(["settings"])],
    )

    write(
        "12-settings-font-missing",
        settings=base_settings(customFontName="Missing", customFontPath="<font>"),
        font={"name": "missing.ttf", "missing": True},
        steps=[export(["settings"])],
    )

    write(
        "13-api-keys",
        api_keys=[
            {"id": "k1", "name": "Google", "key": "secret-1", "provider": "google"},
            {"id": "k2", "name": "Custom", "key": "secret-2", "provider": "custom"},
        ],
        activeApiKeyIds={"google": "k1", "custom": "k2"},
        webSearchApiKeys={"brave": "search-secret"},
        proxyPassword="proxy-secret",
        steps=[
            export(["api_keys"], include_api_keys=True),
            export(["api_keys"], include_api_keys=False),
        ],
    )

    write(
        "14-all-categories",
        seed=[
            conversation("c1", title="all", changed=1),
            run("r1", "c1"),
            message("m1", "c1", "r1", text="all", seq=0),
            loop("c1", prompt="loop", max_cycles=5),
            task(),
        ],
        memory={"active": "active", "files": {"a.md": "A"}, "meta": '{"a.md":"desc"}'},
        skills={"files": {"s.md": "S"}, "meta": '{"s.md":"skill"}'},
        system_prompts=[{"id": "p1", "title": "P", "content": "C"}],
        settings=base_settings(customFontName="F", customFontPath="<font>"),
        font={"name": "f.ttf", "base64": base64.b64encode(b"FONT").decode("ascii")},
        api_keys=[{"id": "k1", "name": "K", "key": "secret", "provider": "google"}],
        steps=[
            export(
                ["conversations", "memories", "system_prompts", "settings", "api_keys"],
                include_api_keys=True,
            )
        ],
    )

    write(
        "15-output-unavailable",
        steps=[export(["settings"], output="unavailable")],
    )

    write(
        "16-loop-sanitize",
        seed=[
            conversation("c1", changed=1),
            conversation("c2", changed=1),
            conversation("c3", changed=1),
            conversation("c4", changed=1),
            conversation("c5", changed=1),
            conversation("c6", changed=1),
            loop("c1", prompt="  Keep going  ", cycle=-3, max_cycles=None),
            loop("c2", prompt="", cycle=2, max_cycles=0),
            loop("c3", prompt="One", cycle=0, max_cycles=1),
            loop("c4", prompt="Hundred", cycle=99, max_cycles=100),
            loop("c5", prompt="Too many", cycle=1, max_cycles=101),
            loop("c6", prompt="   ", cycle=7, max_cycles=5),
        ],
        steps=[export(["conversations"])],
    )

    write(
        "17-media-extensions",
        seed=[
            conversation("c1", changed=1),
            run("r1", "c1"),
            message(
                "m1",
                "c1",
                "r1",
                images=[
                    "file://<media>/images/Photo.PNG?x=1#frag",
                    "<media>/images/noext",
                    "<media>/images/a.toolongextension",
                    "<media>/images/a.p!g",
                    "<media>/images/a.",
                    "   ",
                    "content://media/external/images/9",
                ],
                seq=0,
            ),
            message(
                "m2",
                "c1",
                "r1",
                images=["<media>/images/Photo.PNG", "file://<media>/images/Photo.PNG"],
                seq=1,
            ),
            message(
                "m3",
                "c1",
                "r1",
                images=["<media>/images/missing.png", "content://media/external/images/missing"],
                seq=2,
            ),
        ],
        media=[
            fixture("images/Photo.PNG", "P"),
            fixture("images/noext", "N"),
            fixture("images/a.toolongextension", "L"),
            fixture("images/a.p!g", "G"),
            fixture("images/a.", "D"),
            fixture("images/content-9.png", "9", uri="content://media/external/images/9"),
        ],
        steps=[export(["conversations"])],
    )

    write(
        "18-draft-invalid",
        seed=[
            conversation(
                "c1",
                changed=1,
                draftAttachments="{not json",
            ),
            conversation(
                "c2",
                changed=1,
                draftAttachments=json.dumps(
                    [
                        {
                            "localId": "s1",
                            "uri": "file://<media>/sandbox.png",
                            "type": "image",
                            "storage": "local_sandbox_runtime",
                            "sandboxPath": "/sandbox/x.png",
                        },
                        {
                            "localId": "a1",
                            "uri": "file://<media>/images/keep.png",
                            "type": "image",
                            "fileName": "keep.png",
                        },
                        {
                            "localId": "a2",
                            "uri": "file://<media>/images/gone.png",
                            "type": "image",
                        },
                    ],
                    separators=(",", ":"),
                ),
            ),
            conversation("c3", changed=1, draftAttachments="[]"),
            run("r1", "c1"),
            message(
                "m1",
                "c1",
                "r1",
                images=[],
                attachmentMeta="{bad",
                toolCallJson="[not a segment]",
                seq=0,
            ),
        ],
        media=[fixture("images/keep.png", "K")],
        steps=[export(["conversations"])],
    )

    print(f"Wrote {len(os.listdir(OUT))} scenarios to {OUT}")


if __name__ == "__main__":
    main()
