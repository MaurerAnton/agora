#!/usr/bin/env python3
"""Deterministic scenario generator for the semantic index ledger stand."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def ledger(model: str, **overrides) -> dict:
    entity = {
        "modelId": model,
        "state": "NEEDS_RECONCILE",
        "sourceRevision": 0,
        "completedRevision": 0,
        "reconcileRevision": 0,
        "updatedAt": 0,
    }
    entity.update(overrides)
    return entity


def work(model: str, message: str, fingerprint: str | None, revision: int, updated: int) -> dict:
    return {
        "modelId": model,
        "messageId": message,
        "sourceFingerprint": fingerprint,
        "sourceRevision": revision,
        "updatedAt": updated,
    }


def message(identifier: str, conversation: str, text: str, participant: str = "USER") -> dict:
    return {
        "id": identifier,
        "conversationId": conversation,
        "text": text,
        "participant": participant,
    }


def embedding(identifier: int | None, message_id: str, model: str, blob: str,
              text: str, dimension: int, fingerprint: str | None) -> dict:
    return {
        "id": identifier,
        "messageId": message_id,
        "modelId": model,
        "embedding": blob,
        "chunkText": text,
        "dimension": dimension,
        "sourceFingerprint": fingerprint,
    }


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-admission-and-reconcile", {
        "seed": {"conversations": [{"id": "c1", "taskId": None}]},
        "steps": [
            {"op": "fingerprint", "text": "hello"},
            {"op": "snapshot", "activeModelId": "m1", "configured": ["m1", "", "m2", "m1"]},
            {"op": "admitModel", "modelId": "m1", "updatedAt": 100},
            {"op": "admitModel", "modelId": "m1", "updatedAt": 200},
            {"op": "getLedger", "modelId": "m1"},
            {"op": "requestReconcile", "modelId": "m1", "updatedAt": 300},
            {"op": "getLedger", "modelId": "m1"},
            {"op": "getLedger", "modelId": "missing"},
            {"op": "admitModel", "modelId": "", "updatedAt": 1},
        ],
    })

    write("02-entity-validation", {
        "steps": [
            {"op": "insertLedger", "ledger": ledger("m", state="BROKEN", updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger("m", sourceRevision=-1, updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger(
                "m", sourceRevision=2, completedRevision=3, updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger(
                "m", sourceRevision=2, reconcileRevision=3, updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger(
                "m", state="CURRENT", sourceRevision=1, completedRevision=0, updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger(
                "m", state="CURRENT", sourceRevision=1, completedRevision=1, updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger("m", updatedAt=1)},
            {"op": "insertLedger", "ledger": ledger("m", updatedAt=2)},
            {"op": "upsertWork", "work": work("m", "", None, 1, 1)},
            {"op": "upsertWork", "work": work("m", "x", "  ", 1, 1)},
            {"op": "upsertWork", "work": work("m", "x", None, 0, 1)},
        ],
    })

    write("03-exact-work-lifecycle", {
        "seed": {
            "messages": [message("m1", "c1", "hello"), message("m2", "c1", "world")],
            "conversations": [{"id": "c1", "taskId": None}],
        },
        "steps": [
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "m1",
             "sourceFingerprint": "fp-1", "updatedAt": 10},
            {"op": "getWork", "modelId": "model", "messageId": "m1"},
            {"op": "completeExactWork",
             "work": work("model", "m1", "fp-1", 1, 10), "updatedAt": 20},
            {"op": "getLedger", "modelId": "model"},
            {"op": "completeExactWork",
             "work": work("model", "m1", "fp-1", 1, 10), "updatedAt": 30},
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "m1",
             "sourceFingerprint": "fp-2", "updatedAt": 40},
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "m2",
             "sourceFingerprint": None, "updatedAt": 50},
            {"op": "completeExactWork",
             "work": work("model", "m1", "fp-1", 2, 40), "updatedAt": 60},
            {"op": "getWork", "modelId": "model", "messageId": "m1"},
            {"op": "completeExactWork",
             "work": work("model", "m1", "fp-2", 2, 40), "updatedAt": 70},
            {"op": "getLedger", "modelId": "model"},
            {"op": "completeExactWork",
             "work": work("model", "m2", None, 3, 50), "updatedAt": 80},
            {"op": "getLedger", "modelId": "model"},
            {"op": "enqueueExactWork", "modelId": "", "messageId": "m1",
             "sourceFingerprint": None, "updatedAt": 90},
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "",
             "sourceFingerprint": None, "updatedAt": 90},
        ],
    })

    write("04-reconcile-lifecycle", {
        "seed": {"messages": [message("m1", "c1", "hello")]},
        "steps": [
            {"op": "requestReconcile", "modelId": "model", "updatedAt": 10},
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "m1",
             "sourceFingerprint": "fp", "updatedAt": 20},
            {"op": "completeReconcile", "modelId": "model",
             "expectedReconcileRevision": 1, "updatedAt": 30},
            {"op": "getLedger", "modelId": "model"},
            {"op": "completeReconcile", "modelId": "model",
             "expectedReconcileRevision": 1, "updatedAt": 40},
            {"op": "deleteWorkForModel", "modelId": "model"},
            {"op": "completeReconcile", "modelId": "model",
             "expectedReconcileRevision": 1, "updatedAt": 50},
            {"op": "getLedger", "modelId": "model"},
            {"op": "completeReconcile", "modelId": "model",
             "expectedReconcileRevision": 5, "updatedAt": 60},
            {"op": "completeReconcile", "modelId": "", "expectedReconcileRevision": 1,
             "updatedAt": 70},
            {"op": "completeReconcile", "modelId": "model",
             "expectedReconcileRevision": -1, "updatedAt": 70},
        ],
    })

    write("05-work-pages", {
        "steps": [
            {"op": "requestReconcile", "modelId": "model", "updatedAt": 1},
            {"op": "upsertWork", "work": work("model", "a", "fa", 2, 10)},
            {"op": "upsertWork", "work": work("model", "b", "fb", 1, 11)},
            {"op": "upsertWork", "work": work("model", "c", None, 2, 12)},
            {"op": "upsertWork", "work": work("model", "d", "fd", 3, 13)},
            {"op": "upsertWork", "work": work("other", "a", "fo", 1, 14)},
            {"op": "getWorkPage", "modelId": "model", "maxSourceRevision": 2,
             "afterSourceRevision": 0, "afterMessageId": "", "limit": 2},
            {"op": "getWorkPage", "modelId": "model", "maxSourceRevision": 2,
             "afterSourceRevision": 1, "afterMessageId": "b", "limit": 2},
            {"op": "getWorkPage", "modelId": "model", "maxSourceRevision": 2,
             "afterSourceRevision": 2, "afterMessageId": "c", "limit": 2},
            {"op": "getWorkPage", "modelId": "model", "maxSourceRevision": 3,
             "afterSourceRevision": 0, "afterMessageId": "", "limit": 10},
            {"op": "getWorkCountThroughRevision", "modelId": "model",
             "maxSourceRevision": 2},
            {"op": "getWorkCountThroughRevision", "modelId": "model",
             "maxSourceRevision": 0},
        ],
    })

    write("06-reconcile-page", {
        "seed": {
            "conversations": [
                {"id": "c1", "taskId": None},
                {"id": "task", "taskId": "t1"},
            ],
            "messages": [
                message("m1", "c1", "alpha"),
                message("m2", "c1", "beta", participant="MODEL"),
                message("m3", "c1", ""),
                message("m4", "c1", "tool body"),
                message("tool_1", "c1", "tool"),
                message("result_1", "c1", "result"),
                message("compact_1", "c1", "compact"),
                message("m5", "task", "task message"),
                message("m6", "c1", "gamma", participant="SYSTEM"),
                message("m10", "c1", "delta"),
                message("m11", "c1", "epsilon"),
            ],
            "embeddings": [
                embedding(1, "m1", "model", "0a0b", "alpha", 2, "fp-alpha"),
                embedding(2, "m2", "model", "0c0d0e", "beta", 3, None),
                embedding(3, "m1", "other", "ff", "alpha", 1, "fp-other"),
            ],
        },
        "steps": [
            {"op": "getReconcileMessagesPage", "modelId": "model", "maxWorkRevision": 0,
             "afterId": None, "limit": 10},
            {"op": "getReconcileMessagesPage", "modelId": "model", "maxWorkRevision": 0,
             "afterId": "m1", "limit": 3},
            {"op": "getReconcileMessagesPage", "modelId": "model", "maxWorkRevision": 5,
             "afterId": None, "limit": 10},
            {"op": "upsertWork", "work": work("model", "m1", "fp-alpha", 4, 20)},
            {"op": "upsertWork", "work": work("model", "m10", "fp-delta", 6, 21)},
            {"op": "getReconcileMessagesPage", "modelId": "model", "maxWorkRevision": 4,
             "afterId": None, "limit": 10},
            {"op": "getReconcileMessagesPage", "modelId": "model", "maxWorkRevision": 5,
             "afterId": None, "limit": 10},
            {"op": "getSearchableMessageText", "messageId": "m1"},
            {"op": "getSearchableMessageText", "messageId": "tool_1"},
            {"op": "getSearchableMessageText", "messageId": "m5"},
            {"op": "getSearchableMessageText", "messageId": "missing"},
            {"op": "getSearchableMessageText", "messageId": "m6"},
        ],
    })

    write("07-embedding-upsert-and-deletes", {
        "seed": {
            "messages": [message("m1", "c1", "alpha"), message("m2", "c1", "beta")],
            "embeddings": [
                embedding(7, "m1", "model", "01", "alpha", 1, "fp-1"),
                embedding(8, "m1", "other", "02", "alpha", 1, "fp-1"),
                embedding(9, "m2", "model", "03", "beta", 1, "fp-2"),
            ],
        },
        "steps": [
            {"op": "upsertEmbedding", "embedding": embedding(
                None, "m2", "new-model", "aa", "beta", 1, "fp-2")},
            {"op": "upsertEmbedding", "embedding": embedding(
                7, "m1", "model", "aabb", "alpha2", 2, "fp-1b")},
            {"op": "upsertEmbedding", "embedding": embedding(
                None, "m1", "model", "cccc", "alpha3", 2, "fp-1c")},
            {"op": "upsertEmbedding", "embedding": embedding(
                8, "m1", "other", "dd", "alpha4", 1, "fp-1d")},
            {"op": "getWork", "modelId": "model", "messageId": "m1"},
            {"op": "deleteMatchingWork", "modelId": "model", "messageId": "m1",
             "sourceFingerprint": None, "sourceRevision": 1},
            {"op": "deleteEmbeddingsForMessages", "messageIds": ["m1", "missing"]},
            {"op": "deleteEmbeddingsForModel", "modelId": "model"},
            {"op": "deleteEmbeddingsForMessages", "messageIds": []},
            {"op": "deleteAllEmbeddings"},
        ],
    })

    write("08-model-invalidation", {
        "seed": {
            "messages": [message("m1", "c1", "alpha")],
            "embeddings": [
                embedding(1, "m1", "a", "01", "alpha", 1, "fp"),
                embedding(2, "m1", "b", "02", "alpha", 1, "fp"),
            ],
        },
        "steps": [
            {"op": "requestReconcile", "modelId": "a", "updatedAt": 1},
            {"op": "requestReconcile", "modelId": "b", "updatedAt": 2},
            {"op": "upsertWork", "work": work("a", "m1", "fp", 2, 3)},
            {"op": "invalidateSemanticModel", "modelId": "a", "updatedAt": 10},
            {"op": "deleteSemanticModel", "modelId": "b"},
            {"op": "getLedgers", "modelIds": ["a", "b"]},
            {"op": "getLedgers", "modelIds": []},
        ],
    })

    write("09-fingerprints", {
        "steps": [
            {"op": "fingerprint", "text": ""},
            {"op": "fingerprint", "text": "hello"},
            {"op": "fingerprint", "text": "héllo — 世界"},
            {"op": "fingerprint", "text": "a" * 8000},
            {"op": "fingerprint", "text": "a" * 7999 + "\U0001F600"},
            {"op": "fingerprint", "text": "a" * 8000 + "tail"},
            {"op": "fingerprint", "text": "😀😀😀"},
        ],
    })

    write("10-source-mutation", {
        "seed": {
            "messages": [
                message("m1", "c1", "before"),
                message("m2", "c1", "stable"),
                message("m3", "c1", ""),
            ],
            "embeddings": [
                embedding(1, "m1", "active", "01", "before", 1, "old"),
                embedding(2, "m1", "other", "02", "before", 1, "old"),
                embedding(3, "m2", "active", "03", "stable", 1, "stable"),
            ],
        },
        "steps": [
            {"op": "withSemanticSourceMutation",
             "activeModelId": "active", "configured": ["active", "other"],
             "messageIds": ["m1", "m2", "", "m1"],
             "updatedAt": 100,
             "blockSql": [{"sql": "UPDATE messages SET text = ? WHERE id = ?",
                           "args": ["after", "m1"]}]},
            {"op": "withSemanticSourceMutation",
             "activeModelId": "active", "configured": ["active", "other"],
             "messageIds": ["m1", "m2"],
             "updatedAt": 200,
             "blockSql": [{"sql": "SELECT 1"}]},
            {"op": "withSemanticSourceMutation",
             "activeModelId": "active", "configured": ["active", "other"],
             "messageIds": ["m3"],
             "updatedAt": 300,
             "blockSql": [{"sql": "UPDATE messages SET text = ? WHERE id = ?",
                           "args": ["now indexed", "m3"]}]},
        ],
    })

    write("11-graph-mutation", {
        "seed": {
            "messages": [message("m1", "c1", "alpha"), message("m2", "c1", "beta")],
            "embeddings": [
                embedding(1, "m1", "active", "01", "alpha", 1, "fp1"),
                embedding(2, "m2", "active", "02", "beta", 1, "fp2"),
                embedding(3, "m1", "other", "03", "alpha", 1, "fp1"),
            ],
        },
        "steps": [
            {"op": "withSemanticGraphMutation",
             "activeModelId": "active", "configured": ["active", "other"],
             "clearMessageIds": ["m1", "", "m1"],
             "clearAllEmbeddings": False,
             "updatedAt": 100,
             "blockSql": [{"sql": "SELECT 1"}]},
            {"op": "withSemanticGraphMutation",
             "activeModelId": "active", "configured": ["active"],
             "clearMessageIds": [],
             "clearAllEmbeddings": True,
             "updatedAt": 200,
             "blockSql": [{"sql": "SELECT 1"}]},
        ],
    })

    write("12-eligibility-mutation", {
        "seed": {
            "conversations": [
                {"id": "c1", "taskId": None},
                {"id": "c2", "taskId": None},
                {"id": "task", "taskId": "t1"},
            ],
            "messages": [
                message("m1", "c1", "alpha"),
                message("m2", "c2", "beta"),
                message("m3", "task", "gamma"),
            ],
            "embeddings": [
                embedding(1, "m1", "active", "01", "alpha", 1, "fp1"),
                embedding(2, "m2", "active", "02", "beta", 1, "fp2"),
            ],
        },
        "steps": [
            {"op": "withSemanticEligibilityMutation",
             "activeModelId": "active", "configured": ["active"],
             "conversationId": "c1", "updatedAt": 100,
             "blockSql": [{"sql": "UPDATE conversations SET taskId = ? WHERE id = ?",
                           "args": ["t9", "c1"]}]},
            {"op": "withSemanticEligibilityMutation",
             "activeModelId": "active", "configured": ["active"],
             "conversationId": "c2", "updatedAt": 200,
             "blockSql": [{"sql": "SELECT 1"}]},
            {"op": "withSemanticEligibilityMutation",
             "activeModelId": "active", "configured": ["active"],
             "conversationId": "task", "updatedAt": 300,
             "blockSql": [{"sql": "UPDATE conversations SET taskId = ? WHERE id = ?",
                           "args": [None, "task"]}]},
            {"op": "withSemanticEligibilityMutation",
             "activeModelId": "active", "configured": ["active"],
             "conversationId": "missing", "updatedAt": 400,
             "blockSql": [{"sql": "SELECT 1"}]},
        ],
    })

    write("13-commit-embedding", {
        "seed": {
            "messages": [message("m1", "c1", "alpha"), message("m2", "c1", "beta")],
            "embeddings": [embedding(5, "m1", "model", "01", "alpha", 1, "fp-alpha")],
        },
        "steps": [
            {"op": "requestReconcile", "modelId": "model", "updatedAt": 10},
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "m2",
             "sourceFingerprint": "fp-beta", "updatedAt": 20},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(None, "m2", "model", "aabb", "beta", 2, "fp-beta"),
             "expectedFingerprint": "fp-beta", "updatedAt": 30,
             "expectedWorkRevision": 2, "expectedReconcileRevision": 1,
             "completePendingWork": True},
            {"op": "getLedger", "modelId": "model"},
            {"op": "getWork", "modelId": "model", "messageId": "m2"},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(None, "m2", "model", "ccdd", "beta", 2, "fp-beta"),
             "expectedFingerprint": "fp-wrong", "updatedAt": 40,
             "expectedWorkRevision": 2, "expectedReconcileRevision": 1,
             "completePendingWork": True},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(None, "m2", "model", "ccdd", "beta", 2, "fp-beta"),
             "expectedFingerprint": "fp-beta", "updatedAt": 40,
             "expectedWorkRevision": 9, "expectedReconcileRevision": 1,
             "completePendingWork": True},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(None, "m2", "model", "ccdd", "beta", 2, "fp-beta"),
             "expectedFingerprint": "fp-beta", "updatedAt": 40,
             "expectedWorkRevision": 2, "expectedReconcileRevision": 7,
             "completePendingWork": True},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(None, "m2", "unknown", "ccdd", "beta", 2, "fp-beta"),
             "expectedFingerprint": "fp-beta", "updatedAt": 40,
             "expectedWorkRevision": None, "expectedReconcileRevision": None,
             "completePendingWork": True},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(None, "m1", "model", "eeff", "alpha", 2, "fp-alpha"),
             "expectedFingerprint": "fp-alpha", "updatedAt": 50,
             "expectedWorkRevision": None, "expectedReconcileRevision": 1,
             "completePendingWork": False},
            {"op": "enqueueExactWork", "modelId": "model", "messageId": "m2",
             "sourceFingerprint": "fp-beta", "updatedAt": 60},
            {"op": "commitSemanticEmbedding",
             "embedding": embedding(11, "m2", "model", "1122", "beta two", 2, "fp-beta"),
             "expectedFingerprint": "fp-beta", "updatedAt": 70,
             "expectedWorkRevision": 4, "expectedReconcileRevision": None,
             "completePendingWork": True},
        ],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
