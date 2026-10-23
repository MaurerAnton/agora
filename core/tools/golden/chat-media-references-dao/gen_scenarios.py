#!/usr/bin/env python3
"""Deterministic scenario generator for the chat-media-references-dao stand.

The dataset targets every predicate of data/local/ChatMediaReferencesDao.kt: the two-column
attachment eligibility (`images` must not be '' or '[]'; `attachmentMeta` must be non-null and
non-empty), the optional `instr` path token (raw substring, case-sensitive), the tool-payload
requirement (non-null and non-empty), keyset boundaries, LIMIT edges, and the singleton
new-chat draft row.

Messages, conversations and persist rows are listed in ascending id order: the bounded pages all
have ORDER BY id, and the singleton read is a primary-key lookup.
"""
from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation(identifier: str, draft_attachments: str | None = None) -> dict:
    return {"id": identifier, "draftAttachments": draft_attachments}


def run(conversation_id: str) -> dict:
    return {"id": "run-" + conversation_id, "conversationId": conversation_id,
            "status": "COMPLETED", "startedAt": 100, "lastCheckpointAt": 100,
            "endedAt": 200, "endReason": "MODEL_COMPLETED", "currentPass": 0,
            "legacyAmbiguous": False}


def message(identifier: str, conversation_id: str, *, images: list[str] | None = None,
            images_raw: str | None = None, attachment_meta: str | None = None,
            tool_call_json: str | None = None) -> dict:
    row: dict = {"id": identifier, "conversationId": conversation_id,
                 "runId": "run-" + conversation_id}
    if images_raw is not None:
        row["imagesRaw"] = images_raw
    else:
        row["images"] = images or []
    if attachment_meta is not None:
        row["attachmentMeta"] = attachment_meta
    if tool_call_json is not None:
        row["toolCallJson"] = tool_call_json
    return row


def seed(conversations: list[dict], messages: list[dict] | None = None,
         new_chat_persist: list[dict] | None = None) -> dict:
    return {
        "conversations": conversations,
        "runs": [run(row["id"]) for row in conversations],
        "messages": messages or [],
        "newChatPersist": new_chat_persist or [],
    }


def op(name: str, **arguments) -> dict:
    step = {"op": name}
    step.update(arguments)
    return step


def attachments(after_id: str | None, limit: int, path_token: str | None = None) -> dict:
    arguments: dict = {"afterId": after_id, "limit": limit}
    if path_token is not None:
        arguments["pathToken"] = path_token
    return op("getMessageAttachmentReferencesPage", **arguments)


def conversation_attachments(conversation_id: str, after_id: str | None, limit: int) -> dict:
    return op("getConversationMessageAttachmentReferencesPage", conversationId=conversation_id,
              afterId=after_id, limit=limit)


def tool_media(after_id: str | None, limit: int) -> dict:
    return op("getMessageToolMediaReferencesPage", afterId=after_id, limit=limit)


def draft_attachments(after_id: str | None, limit: int,
                      path_token: str | None = None) -> dict:
    arguments: dict = {"afterId": after_id, "limit": limit}
    if path_token is not None:
        arguments["pathToken"] = path_token
    return op("getConversationDraftAttachmentReferencesPage", **arguments)


def new_chat(path_token: str | None = None) -> dict:
    if path_token is None:
        return op("getNewChatDraftAttachmentReference")
    return op("getNewChatDraftAttachmentReference", pathToken=path_token)


def main() -> None:
    write("01-message-attachment-pages", {
        "seed": seed(
            [conversation("c-1")],
            [
                message("m-01", "c-1", images=["a.png"]),
                message("m-02", "c-1", images=[],
                        attachment_meta='{"files":[{"path":"b.png"}]}'),
                message("m-03", "c-1", images=[]),
                message("m-04", "c-1", images=[], attachment_meta=""),
                message("m-05", "c-1", images_raw=""),
                message("m-06", "c-1", images_raw="[]", attachment_meta="{}"),
                message("m-07", "c-1", images=["c.png", "d.png"], attachment_meta=""),
                message("m-08", "c-1", images_raw="legacy1|||legacy2"),
                message("m-09", "c-1", images=["e.png"], attachment_meta="  "),
                message("m-10", "c-1", images=["f.png"]),
            ],
        ),
        "steps": [
            attachments(None, 100),
            attachments(None, 3),
            attachments("m-02", 100),
            attachments("m-10", 100),
            attachments("", 100),
            attachments(None, 0),
            attachments(None, 100, "c.png"),
            attachments(None, 100, "b.png"),
            attachments(None, 100, "legacy2"),
            attachments(None, 100, "missing"),
            attachments(None, 100, "c"),
            attachments(None, 100, "A.PNG"),
            attachments(None, 100, "  "),
            attachments(None, 100, "["),
        ],
    })
    write("02-conversation-attachment-pages", {
        "seed": seed(
            [conversation("c-1"), conversation("c-2")],
            [
                message("m-01", "c-1", images=["a.png"]),
                message("m-02", "c-1", images=[], attachment_meta="{}"),
                message("m-03", "c-1", images=[]),
                message("m-04", "c-1", images=["b.png"]),
                message("m-05", "c-2", images=["c.png"]),
                message("m-06", "c-2", images=[], attachment_meta="{}"),
            ],
        ),
        "steps": [
            conversation_attachments("c-1", None, 100),
            conversation_attachments("c-1", "m-02", 100),
            conversation_attachments("c-1", None, 1),
            conversation_attachments("c-2", None, 100),
            conversation_attachments("c-3", None, 100),
            conversation_attachments("c-1", "m-04", 100),
            conversation_attachments("c-1", "", 100),
        ],
    })
    write("03-tool-media-pages", {
        "seed": seed(
            [conversation("c-1")],
            [
                message("m-01", "c-1", tool_call_json='{"segments":[{"type":"citation"}]}'),
                message("m-02", "c-1"),
                message("m-03", "c-1", tool_call_json=""),
                message("m-04", "c-1", tool_call_json='{"tool":"x"}'),
                message("m-05", "c-1", tool_call_json="   "),
            ],
        ),
        "steps": [
            tool_media(None, 100),
            tool_media("m-01", 100),
            tool_media(None, 1),
            tool_media("m-05", 100),
            tool_media(None, 0),
            tool_media("", 100),
        ],
    })
    write("04-conversation-draft-pages", {
        "seed": seed([
            conversation("c-01", '["a.png"]'),
            conversation("c-02", None),
            conversation("c-03", ""),
            conversation("c-04", '{"draft":"b.png"}'),
            conversation("c-05", "legacy|||format"),
        ]),
        "steps": [
            draft_attachments(None, 100),
            draft_attachments(None, 100, "a.png"),
            draft_attachments(None, 100, "b.png"),
            draft_attachments(None, 100, "legacy"),
            draft_attachments(None, 100, "A.png"),
            draft_attachments("c-01", 100),
            draft_attachments(None, 2),
            draft_attachments(None, 0),
            draft_attachments("", 100),
        ],
    })
    write("05-new-chat-draft", {
        "seed": seed([conversation("c-1")], [],
                     [{"id": 0, "draftAttachments": '["new.png"]',
                       "modelId": "mdl-1", "systemPromptId": "sp-1",
                       "conversationSettingsJson": '{"temperature":0.2}',
                       "draftText": "hello"}]),
        "steps": [
            new_chat(),
            new_chat("new.png"),
            new_chat("NEW.PNG"),
            new_chat("missing"),
            new_chat(""),
        ],
    })
    write("06-new-chat-draft-empty", {
        "seed": seed([conversation("c-1")], [], [{"id": 0, "draftAttachments": ""}]),
        "steps": [new_chat(), new_chat("x")],
    })
    write("07-new-chat-draft-null", {
        "seed": seed([conversation("c-1")], [], [{"id": 0, "draftAttachments": None}]),
        "steps": [new_chat()],
    })
    write("08-new-chat-draft-absent", {
        "seed": seed([conversation("c-1")]),
        "steps": [new_chat(), new_chat("x")],
    })


if __name__ == "__main__":
    main()
