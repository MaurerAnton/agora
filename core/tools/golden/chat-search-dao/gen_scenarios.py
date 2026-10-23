#!/usr/bin/env python3
"""Deterministic scenario generator for the chat-search-dao stand.

One shared dataset exercises every predicate of data/local/ChatSearchDao.kt: task-owned
conversations (including an empty-string taskId, which is NOT NULL), ERROR participants, empty
message text, the `tool_`/`result_`/`compact_` id boundaries, LIKE escape semantics, ASCII
case folding, unicode LENGTH, the `_` wildcard inside the NOT LIKE prefixes, citation JSON
substring matching, and keyset/offset paging edges.

Messages, conversations and embeddings are listed in ascending id order: two of the DAO members
have no ORDER BY, so the seed order keeps the rowid order and the id order identical and the
traces do not depend on a query plan.
"""
from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

CITATION = '{"toolCallId":"tc","segments":[{"type":"citation","citationId":"cit-1"}]}'
SPACED_CITATION = '{"segments":[{"type": "citation"}]}'


def write(name: str, scenario: dict) -> None:
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation(identifier: str, title: str, last_updated: int, **fields) -> dict:
    row = {"id": identifier, "title": title, "lastUpdated": last_updated}
    row.update(fields)
    return row


def run(identifier: str, conversation_id: str) -> dict:
    return {"id": identifier, "conversationId": conversation_id, "status": "COMPLETED",
            "activeSlot": None, "startedAt": 100, "lastCheckpointAt": 100,
            "endedAt": 200, "endReason": "MODEL_COMPLETED", "currentPass": 0,
            "legacyAmbiguous": False}


def message(identifier: str, conversation_id: str, text: str, participant: str = "MODEL",
            timestamp: int = 0, **fields) -> dict:
    row = {"id": identifier, "conversationId": conversation_id, "text": text,
           "participant": participant, "timestamp": timestamp,
           "runId": "r-" + conversation_id[2:]}
    row.update(fields)
    return row


def embedding(identifier: int, message_id: str, model_id: str, payload: str,
              dimension: int) -> dict:
    return {"id": identifier, "messageId": message_id, "modelId": model_id,
            "embedding": payload, "chunkText": "chunk-" + str(identifier),
            "dimension": dimension, "sourceFingerprint": "fp-" + str(identifier)}


def op(name: str, **arguments) -> dict:
    step = {"op": name}
    step.update(arguments)
    return step


CONVERSATIONS = [
    conversation("c-alpha", "Alpha Notes", 300, dataChangedAt=301,
                 selectedBranchesJson='{"m":"x"}', systemPromptId="sp-1",
                 modelId="mdl-1", origin="user", draftText="draft",
                 draftAttachments="[]", selectedRunBranchesJson='{"r":"y"}',
                 hasUnreadGeneration=True, isPinned=True),
    conversation("c-beta", "Project Emerald", 100, origin="loop", graduated=True),
    conversation("c-delta", "Delta `code` 100%", 250, dataChangedAt=250, isPinned=True),
    conversation("c-epsilon", "Epsilon", 900, dataChangedAt=900),
    conversation("c-gamma", "Gamma 100% coverage", 250, dataChangedAt=250),
    conversation("c-task", "Secret Alpha Task", 50, taskId="task-1", origin="task",
                 graduated=True),
    conversation("c-zeta", "Zeta", 10, taskId=""),
]

RUNS = [run("r-" + row["id"][2:], row["id"]) for row in CONVERSATIONS]

MESSAGES = [
    message("compact", "c-alpha", "alpha compact boundary", timestamp=282,
            parentId="m-01", images=["file://a.png"], thoughts="thinking",
            thoughtTitle="plan", tokenCount=12, inputTokenCount=3,
            cachedInputTokenCount=1, cacheWriteInputTokenCount=2,
            uncachedInputTokenCount=0, outputTokenCount=4, reasoningTokenCount=5,
            generationDurationMs=1234, thoughtTimeMs=99, modelName="model-x",
            attachmentMeta='{"files":[]}', runSequence=7, consumedAtPass=2,
            sourceJson='{"kind":"edited"}'),
    message("compact_09", "c-alpha", "Alpha compact summary", timestamp=960,
            toolCallJson=CITATION),
    message("m-01", "c-alpha", "The alpha build ships today", timestamp=500,
            inputTokenCount=10, outputTokenCount=20),
    message("m-02", "c-alpha", "Alpha is the first letter", participant="USER", timestamp=500),
    message("m-03", "c-beta", "Unrelated body", timestamp=400),
    message("m-04", "c-task", "Alpha in a task conversation", timestamp=600),
    message("m-05", "c-alpha", "Alpha from an error participant", participant="ERROR",
            timestamp=700),
    message("m-06", "c-alpha", "", timestamp=800),
    message("m-07", "c-alpha", "Alpha again", timestamp=500),
    message("m-08", "c-beta", "Alpha from loop origin", participant="USER", timestamp=60),
    message("m-09", "c-alpha", "alpha", timestamp=70),
    message("m-10", "c-gamma", "Lowercase alpha appears", participant="USER", timestamp=100),
    message("m-11", "c-epsilon", "100% coverage complete", timestamp=110),
    message("m-12", "c-epsilon", "100x coverage failed", timestamp=120),
    message("m-13", "c-delta", "value a_b here", timestamp=130),
    message("m-14", "c-delta", "value axb here", timestamp=140),
    message("m-15", "c-alpha", "path C:\\temp\\alpha", timestamp=150),
    message("m-16", "c-alpha", "citation holder", timestamp=160, toolCallJson=CITATION),
    message("m-17", "c-beta", "spaced citation", timestamp=170, toolCallJson=SPACED_CITATION),
    message("m-18", "c-task", "citation in task", timestamp=180, toolCallJson=CITATION),
    message("m-19", "c-alpha", "citation user", participant="USER", timestamp=190,
            toolCallJson=CITATION),
    message("m-20", "c-gamma", "citation one", timestamp=200, toolCallJson=CITATION),
    message("m-21", "c-gamma", "citation two", timestamp=210, toolCallJson=CITATION),
    message("m-22", "c-alpha", "citation three", timestamp=220, toolCallJson=CITATION),
    message("m-23", "c-gamma", "éé", participant="USER", timestamp=230),
    message("m-24", "c-gamma", "漢字", participant="USER", timestamp=240),
    message("m-25", "c-gamma", "é", participant="USER", timestamp=250),
    message("m-26", "c-gamma", "😀", participant="USER", timestamp=260),
    message("m-27", "c-epsilon", "tools are useful", timestamp=270),
    message("m-28", "c-epsilon", "ALPHA", timestamp=80),
    message("m-29", "c-zeta", "Alpha in empty-task conversation", timestamp=290),
    message("m-30", "c-alpha", "citation four", timestamp=300, toolCallJson=CITATION),
    message("m-31", "c-alpha", "citation five", timestamp=310, toolCallJson=CITATION),
    message("m-35", "c-alpha", "citation compact exact", timestamp=90, toolCallJson=CITATION),
    message("result_08", "c-alpha", "Alpha result output", timestamp=950),
    message("resultx", "c-alpha", "alpha resultx boundary", timestamp=281),
    message("tool_07", "c-alpha", "Alpha tool output", timestamp=900),
    message("toolx", "c-epsilon", "alpha with toolx id", timestamp=280),
]

EMBEDDINGS = [
    embedding(1, "m-01", "model-a", "0a0b", 2),
    embedding(2, "m-02", "model-a", "0c", 1),
    embedding(3, "m-03", "model-a", "0d0e", 2),
    embedding(4, "m-04", "model-a", "0f", 1),
    embedding(5, "m-05", "model-a", "10", 1),
    embedding(6, "m-06", "model-a", "11", 1),
    embedding(7, "tool_07", "model-a", "12", 1),
    embedding(8, "result_08", "model-a", "13", 1),
    embedding(9, "compact_09", "model-a", "14", 1),
    embedding(10, "m-10", "model-a", "15", 1),
    embedding(11, "m-11", "model-a", "16", 1),
    embedding(12, "m-12", "model-a", "17", 1),
    embedding(13, "m-13", "model-a", "18", 1),
    embedding(14, "m-14", "model-a", "19", 1),
    embedding(15, "m-16", "model-a", "1a", 1),
    embedding(16, "m-20", "model-a", "1b", 1),
    embedding(17, "m-21", "model-a", "1c", 1),
    embedding(18, "m-22", "model-a", "1d", 1),
    embedding(19, "m-23", "model-a", "1e", 1),
    embedding(20, "m-24", "model-a", "1f", 1),
    embedding(21, "m-25", "model-a", "20", 1),
    embedding(22, "m-26", "model-a", "21", 1),
    embedding(23, "m-27", "model-a", "22", 1),
    embedding(24, "m-28", "model-a", "23", 1),
    embedding(25, "m-30", "model-a", "24", 1),
    embedding(26, "m-31", "model-a", "25", 1),
    embedding(27, "m-35", "model-a", "26", 1),
    embedding(28, "resultx", "model-a", "27", 1),
    embedding(29, "toolx", "model-a", "28", 1),
    embedding(30, "m-01", "model-b", "29", 1),
    embedding(31, "m-02", "model-b", "2a", 1),
    embedding(32, "m-09", "model-b", "2b", 1),
    embedding(33, "m-10", "model-b", "2c", 1),
    embedding(34, "m-01", "model-c", "2d", 1),
    embedding(35, "m-29", "model-a", "2e", 1),
    embedding(36, "m-18", "model-a", "2f", 1),
    embedding(37, "m-04", "model-b", "30", 1),
]

SEED = {
    "conversations": CONVERSATIONS,
    "runs": RUNS,
    "messages": MESSAGES,
    "embeddings": EMBEDDINGS,
}


def scenario(steps: list[dict]) -> dict:
    return {"seed": SEED, "steps": steps}


def search(query: str, limit: int | None = None) -> dict:
    arguments = {"query": query}
    if limit is not None:
        arguments["limit"] = limit
    return op("searchMessages", **arguments)


def citations(after_id: str, page_size: int) -> dict:
    return op("getMessagesWithCitationSegmentsPage", afterId=after_id, pageSize=page_size)


def embedding_page(model_id: str, after_id: int, minimum_text_length: int,
                   limit: int) -> dict:
    return op("getEmbeddingSearchPage", modelId=model_id, afterId=after_id,
              minimumTextLength=minimum_text_length, limit=limit)


def indexable_page(after_id: str | None, limit: int) -> dict:
    return op("getSearchableMessagesPage", afterId=after_id, limit=limit)


def unembedded_page(model_id: str, after_id: str | None, limit: int) -> dict:
    return op("getUnembeddedMessagesPage", modelId=model_id, afterId=after_id, limit=limit)


def by_ids(ids: list[str]) -> dict:
    return op("getSearchableMessagesByIds", ids=ids)


def conversations_page(direction: str, offset: int, limit: int) -> dict:
    return op("getSearchableConversationsPage" + direction, offset=offset, limit=limit)


def main() -> None:
    write("01-search-text-and-title", scenario([
        search("alpha", 100),
        search("ALPHA", 100),
        search("Alpha", 4),
        search("emerald", 100),
        search("emerald", 0),
        search("", 100),
        search("nonexistent", 100),
        search("alpha"),
    ]))
    write("02-search-like-escapes", scenario([
        search("100\\%", 100),
        search("100%", 100),
        search("a\\_b", 100),
        search("a_b", 100),
        search("C:\\\\temp\\\\alpha", 100),
        search("\\\\", 100),
        search("100\\% coverage", 100),
    ]))
    write("03-citation-pages", scenario([
        citations("", 100),
        citations("", 3),
        citations("m-16", 2),
        citations("m-22", 100),
        citations("m-31", 100),
        citations("compact_09", 100),
        citations("zzz", 100),
    ]))
    write("04-embedding-pages", scenario([
        embedding_page("model-a", 0, 0, 100),
        embedding_page("model-a", 0, 2, 100),
        embedding_page("model-a", 0, 5, 100),
        embedding_page("model-a", 15, 0, 3),
        embedding_page("model-a", 1000, 0, 100),
        embedding_page("model-b", 0, 0, 100),
        embedding_page("model-c", 0, 0, 100),
        embedding_page("model-a", 0, 0, 0),
    ]))
    write("05-embedding-counts", scenario([
        op("getEmbeddingCountByModel", modelId="model-a"),
        op("getEmbeddingCountByModel", modelId="model-b"),
        op("getEmbeddingCountByModel", modelId="model-c"),
        op("getEmbeddingCountByModel", modelId="model-unknown"),
        op("getEmbeddingCountsByModels", modelIds=["model-a", "model-b", "model-c"]),
        op("getEmbeddingCountsByModels", modelIds=["model-b", "model-a"]),
        op("getEmbeddingCountsByModels", modelIds=[]),
        op("getEmbeddingCountsByModels", modelIds=["model-unknown"]),
        op("getIndexableMessageCount"),
    ]))
    write("06-indexable-pages", scenario([
        indexable_page(None, 100),
        indexable_page(None, 5),
        indexable_page("m-10", 100),
        indexable_page("m-10", 3),
        indexable_page("", 100),
        indexable_page("zzz", 100),
        indexable_page(None, 0),
        unembedded_page("model-a", None, 100),
        unembedded_page("model-b", None, 100),
        unembedded_page("model-a", "m-09", 100),
        unembedded_page("model-a", None, 2),
        unembedded_page("model-c", None, 100),
    ]))
    write("07-messages-by-ids", scenario([
        by_ids(["m-01", "m-16", "m-10", "m-05"]),
        by_ids(["m-01", "m-01", "m-02"]),
        by_ids(["m-04", "m-05", "m-06"]),
        by_ids(["compact_09", "result_08", "tool_07", "resultx", "toolx", "compact"]),
        by_ids([]),
        by_ids(["missing"]),
    ]))
    write("08-searchability", scenario([
        op("isMessageSearchable", messageId="m-01"),
        op("isMessageSearchable", messageId="m-05"),
        op("isMessageSearchable", messageId="m-06"),
        op("isMessageSearchable", messageId="m-04"),
        op("isMessageSearchable", messageId="m-29"),
        op("isMessageSearchable", messageId="compact"),
        op("isMessageSearchable", messageId="compact_09"),
        op("isMessageSearchable", messageId="resultx"),
        op("isMessageSearchable", messageId="toolx"),
        op("isMessageSearchable", messageId="tool_07"),
        op("isMessageSearchable", messageId="missing"),
    ]))
    write("09-conversation-pages", scenario([
        op("getSearchableConversation", conversationId="c-alpha"),
        op("getSearchableConversation", conversationId="c-beta"),
        op("getSearchableConversation", conversationId="c-task"),
        op("getSearchableConversation", conversationId="c-zeta"),
        op("getSearchableConversation", conversationId="nope"),
        op("getSearchableConversationCount"),
        conversations_page("Ascending", 0, 100),
        conversations_page("Ascending", 0, 2),
        conversations_page("Ascending", 2, 2),
        conversations_page("Ascending", 10, 5),
        conversations_page("Ascending", 0, 0),
        conversations_page("Descending", 0, 100),
        conversations_page("Descending", 0, 2),
        conversations_page("Descending", 2, 2),
        conversations_page("Descending", 10, 5),
        conversations_page("Descending", 0, 0),
    ]))


if __name__ == "__main__":
    main()
