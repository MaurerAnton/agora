#!/usr/bin/env python3
"""Deterministic scenario generator for the maintenance debt stand."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

# Above any plausible wall-clock timestamp, so the repair's clock write stays masked while the
# MAX(dataChangedAt + 1, :at) branch remains observable.
FUTURE = 5_000_000_000_000


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def debt(kind: str, identity: str, state: str = "PENDING", revision: int = 1,
         updated: int = 0, claim_id: str | None = None,
         claimed_at: int | None = None) -> dict:
    entity = {"kind": kind, "identity": identity, "state": state, "revision": revision,
              "updatedAt": updated}
    if claim_id is not None:
        entity["claimId"] = claim_id
    if claimed_at is not None:
        entity["claimedAt"] = claimed_at
    return entity


def conversation(identifier: str, run_branches_json: str | None = None,
                 data_changed_at: int = 100) -> dict:
    return {"id": identifier, "selectedRunBranchesJson": run_branches_json,
            "dataChangedAt": data_changed_at}


def run(identifier: str, conversation_id: str, status: str, parent: str | None = None,
        active_slot: int | None = None, started: int = 1000, stopped: int | None = None,
        ended: int | None = None, end_reason: str | None = None, current_pass: int = 0,
        legacy_ambiguous: bool = False) -> dict:
    entity = {"id": identifier, "conversationId": conversation_id, "status": status,
              "startedAt": started, "lastCheckpointAt": started, "currentPass": current_pass,
              "legacyAmbiguous": legacy_ambiguous}
    if parent is not None:
        entity["parentRunId"] = parent
    if active_slot is not None:
        entity["activeSlot"] = active_slot
    if stopped is not None:
        entity["stopRequestedAt"] = stopped
    if ended is not None:
        entity["endedAt"] = ended
    if end_reason is not None:
        entity["endReason"] = end_reason
    return entity


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-enqueue-lifecycle", {
        "steps": [
            {"op": "hasDebt"},
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "c1", "updatedAt": 10},
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "c1", "updatedAt": 20},
            {"op": "enqueue", "kind": "EMBEDDING_ORPHANS", "identity": "*", "updatedAt": 30},
            {"op": "hasDebt"},
            {"op": "getDebt", "kind": "ATTACHMENT_ORPHANS", "identity": "c1"},
            {"op": "getDebt", "kind": "RUN_BRANCHES", "identity": "c9"},
            {"op": "enqueue", "kind": "", "identity": "c1", "updatedAt": 40},
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "  ", "updatedAt": 40},
            {"op": "renewDebt", "kind": "EMBEDDING_ORPHANS", "identity": "*", "updatedAt": 50},
            {"op": "renewDebt", "kind": "MISSING", "identity": "*", "updatedAt": 50},
            {"op": "getClaimCandidates", "staleBefore": 0, "limit": 10},
        ],
    })

    write("02-entity-validation", {
        "steps": [
            {"op": "insertDebt", "debt": debt("", "c1", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "  ", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", state="BROKEN", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", revision=0, updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", claim_id="c-only", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", state="CLAIMED", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", claim_id="c1", claimed_at=5, updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", updated=1)},
            {"op": "insertDebt", "debt": debt("K", "id", updated=2)},
        ],
    })

    write("03-claim-ordering", {
        "steps": [
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "b", "updatedAt": 30},
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "a", "updatedAt": 30},
            {"op": "enqueue", "kind": "EMBEDDING_ORPHANS", "identity": "*", "updatedAt": 10},
            {"op": "enqueue", "kind": "RUN_BRANCHES", "identity": "z", "updatedAt": 20},
            {"op": "claim", "claimId": "claim-1", "claimedAt": 100, "staleBefore": 50,
             "limit": 10},
            {"op": "getClaimCandidates", "staleBefore": 100, "limit": 10},
            {"op": "claim", "claimId": "", "claimedAt": 100, "staleBefore": 50, "limit": 1},
            {"op": "claim", "claimId": "c", "claimedAt": 40, "staleBefore": 50, "limit": 1},
            {"op": "claim", "claimId": "c", "claimedAt": 100, "staleBefore": 50, "limit": 0},
            {"op": "claim", "claimId": "c", "claimedAt": 100, "staleBefore": 50, "limit": 101},
            {"op": "sql", "sql": "SELECT 1"},
        ],
    })

    write("04-claim-stale-revival", {
        "steps": [
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "fresh", "updatedAt": 90},
            {"op": "enqueue", "kind": "EMBEDDING_ORPHANS", "identity": "stale",
             "updatedAt": 10},
            {"op": "claim", "claimId": "first", "claimedAt": 60, "staleBefore": 50,
             "limit": 10},
            {"op": "claim", "claimId": "second", "claimedAt": 120, "staleBefore": 75,
             "limit": 10},
            {"op": "claim", "claimId": "third", "claimedAt": 40, "staleBefore": 40,
             "limit": 10},
        ],
    })

    write("05-complete-release", {
        "steps": [
            {"op": "enqueue", "kind": "ATTACHMENT_ORPHANS", "identity": "c1", "updatedAt": 10},
            {"op": "enqueue", "kind": "RUN_BRANCHES", "identity": "c2", "updatedAt": 11},
            {"op": "claim", "claimId": "claim-1", "claimedAt": 100, "staleBefore": 50,
             "limit": 10},
            {"op": "complete", "debt": debt("ATTACHMENT_ORPHANS", "c1", state="CLAIMED",
                                            revision=1, updated=10, claim_id="claim-1",
                                            claimed_at=100)},
            {"op": "complete", "debt": debt("ATTACHMENT_ORPHANS", "c1", state="CLAIMED",
                                            revision=1, updated=10, claim_id="claim-1",
                                            claimed_at=100)},
            {"op": "release", "debt": debt("RUN_BRANCHES", "c2", state="CLAIMED", revision=1,
                                           updated=11, claim_id="claim-1", claimed_at=100),
             "updatedAt": 99},
            {"op": "complete", "debt": debt("RUN_BRANCHES", "c2", updated=11)},
            {"op": "release", "debt": debt("RUN_BRANCHES", "c2", state="CLAIMED", revision=1,
                                           updated=11, claim_id="claim-1", claimed_at=100),
             "updatedAt": 120},
            {"op": "release", "debt": debt("RUN_BRANCHES", "c2", state="CLAIMED", revision=1,
                                           updated=11, claim_id="claim-1", claimed_at=100),
             "updatedAt": 130},
            {"op": "enqueue", "kind": "EMBEDDING_ORPHANS", "identity": "*", "updatedAt": 140},
            {"op": "claim", "claimId": "claim-2", "claimedAt": 200, "staleBefore": 150,
             "limit": 10},
            {"op": "sql", "sql": "UPDATE maintenance_debt SET revision = revision + 1 " +
             "WHERE kind = 'EMBEDDING_ORPHANS'", "args": []},
            {"op": "complete", "debt": debt("EMBEDDING_ORPHANS", "*", state="CLAIMED",
                                            revision=1, updated=140, claim_id="claim-2",
                                            claimed_at=200)},
            {"op": "release", "debt": debt("EMBEDDING_ORPHANS", "*", state="CLAIMED",
                                           revision=1, updated=140, claim_id="claim-2",
                                           claimed_at=200),
             "updatedAt": 300},
            {"op": "completeClaimed", "kind": "EMBEDDING_ORPHANS", "identity": "*",
             "revision": 2, "claimId": "claim-2"},
            {"op": "completeClaimed", "kind": "EMBEDDING_ORPHANS", "identity": "*",
             "revision": 2, "claimId": "other"},
            {"op": "releaseClaimed", "kind": "EMBEDDING_ORPHANS", "identity": "*",
             "revision": 2, "claimId": "claim-2", "updatedAt": 400},
            {"op": "releaseClaimed", "kind": "EMBEDDING_ORPHANS", "identity": "*",
             "revision": 99, "claimId": "claim-2", "updatedAt": 401},
        ],
    })

    write("06-orphan-pages", {
        "seed": {
            "messages": [
                {"id": "m1", "conversationId": "c1", "text": "alpha"},
                {"id": "m2", "conversationId": "c1", "text": "beta"},
            ],
            "embeddings": [
                {"id": 1, "messageId": "m1", "modelId": "a", "embedding": "01"},
                {"id": 2, "messageId": "compact_9", "modelId": "a", "embedding": "02"},
                {"id": 3, "messageId": "ghost", "modelId": "a", "embedding": "03"},
                {"id": 4, "messageId": "ghost", "modelId": "b", "embedding": "04"},
                {"id": 5, "messageId": "m2", "modelId": "a", "embedding": "05"},
                {"id": 6, "messageId": "lost", "modelId": "a", "embedding": "06"},
            ],
        },
        "steps": [
            {"op": "getOrphanEmbeddingIdsPage", "afterId": 0, "limit": 3},
            {"op": "getOrphanEmbeddingIdsPage", "afterId": 3, "limit": 3},
            {"op": "getOrphanEmbeddingIdsPage", "afterId": 6, "limit": 3},
            {"op": "deleteOrphanEmbeddingsForMessage", "messageId": "ghost"},
            {"op": "deleteOrphanEmbeddingsForMessage", "messageId": "m1"},
            {"op": "deleteOrphanEmbeddingsByIds", "ids": [2]},
            {"op": "deleteOrphanEmbeddingsByIds", "ids": []},
            {"op": "deleteOrphanEmbeddingsByIds", "ids": [99]},
            {"op": "getOrphanEmbeddingIdsPage", "afterId": 0, "limit": 10},
        ],
    })

    write("07-run-branch-pages", {
        "seed": {
            "conversations": [
                conversation("c1", '{"r1":"r2"}'),
                conversation("c2", None),
                conversation("c3", ""),
                conversation("c4", "{}"),
                conversation("bad-pass", "{}"),
                conversation("bad-status", "{}"),
            ],
            "runs": [
                run("r1", "c1", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("r2", "c1", "ACTIVE", parent="r1", active_slot=1),
                run("x1", "c3", "FAILED", active_slot=None, ended=3000,
                    end_reason="PROVIDER_ERROR"),
                run("bad", "bad-pass", "COMPLETED", active_slot=None, ended=4000,
                    end_reason="MODEL_COMPLETED", current_pass=-1),
                run("weird", "bad-status", "BOGUS"),
            ],
        },
        "steps": [
            {"op": "getRunBranchConversationIdsPage", "afterId": None, "limit": 10},
            {"op": "getRunBranchConversationIdsPage", "afterId": "c1", "limit": 1},
            {"op": "getRunBranchConversationIdsPage", "afterId": "zzz", "limit": 5},
            {"op": "getMaintenanceConversation", "conversationId": "c1"},
            {"op": "getMaintenanceConversation", "conversationId": "missing"},
            {"op": "getMaintenanceRuns", "conversationId": "c1"},
            {"op": "getMaintenanceRuns", "conversationId": "missing"},
            {"op": "getMaintenanceRuns", "conversationId": "bad-pass"},
            {"op": "getMaintenanceRuns", "conversationId": "bad-status"},
        ],
    })

    write("08-repair-missing-and-null", {
        "seed": {
            "conversations": [conversation("present", None)],
        },
        "steps": [
            {"op": "repairRunBranches", "conversationId": "absent"},
            {"op": "repairRunBranches", "conversationId": "present"},
        ],
    })

    write("09-repair-malformed", {
        "seed": {
            "conversations": [
                conversation("array", "[1, 2]"),
                conversation("number", "5"),
                conversation("not-a-string-map", '{"a": 1}'),
                conversation("empty", ""),
            ],
        },
        "steps": [
            {"op": "repairRunBranches", "conversationId": "array"},
            {"op": "repairRunBranches", "conversationId": "number"},
            {"op": "repairRunBranches", "conversationId": "not-a-string-map"},
            {"op": "repairRunBranches", "conversationId": "empty"},
        ],
    })

    write("10-repair-valid-noop", {
        "seed": {
            "conversations": [
                conversation("keep", '{"r1":"r2"}'),
                conversation("blank", "{}"),
            ],
            "runs": [
                run("r1", "keep", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("r2", "keep", "ACTIVE", parent="r1", active_slot=1),
            ],
        },
        "steps": [
            {"op": "repairRunBranches", "conversationId": "keep"},
            {"op": "repairRunBranches", "conversationId": "blank"},
        ],
    })

    write("11-repair-invalid-edges", {
        "seed": {
            "conversations": [
                conversation("mixed", '{"r1":"r2","r2":"r2","rX":"r2"}'),
                conversation("null-keys", '{"null":"n1","null":"n2","n1":"n3"}'),
                conversation("self-null", '{"null":"s1","s1":"s1"}'),
                conversation("future-clock", '{"f1":"f2","f9":"f2"}',
                             data_changed_at=FUTURE),
            ],
            "runs": [
                run("r1", "mixed", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("r2", "mixed", "ACTIVE", parent="r1", active_slot=1),
                run("r3", "mixed", "COMPLETED", parent="r1", active_slot=None, ended=2100,
                    end_reason="MODEL_COMPLETED"),
                run("n1", "null-keys", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("n2", "null-keys", "ACTIVE", parent="n1", active_slot=1),
                run("n3", "null-keys", "COMPLETED", parent="n1", active_slot=None, ended=2100,
                    end_reason="MODEL_COMPLETED"),
                run("s1", "self-null", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("f1", "future-clock", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("f2", "future-clock", "ACTIVE", parent="f1", active_slot=1),
            ],
        },
        "steps": [
            {"op": "repairRunBranches", "conversationId": "mixed"},
            {"op": "repairRunBranches", "conversationId": "null-keys"},
            {"op": "repairRunBranches", "conversationId": "self-null"},
            {"op": "repairRunBranches", "conversationId": "future-clock"},
        ],
    })

    write("12-repair-then-observe", {
        "seed": {
            "conversations": [conversation("twice", '{"r1":"r2","r2":"r2"}', data_changed_at=100)],
            "runs": [
                run("r1", "twice", "COMPLETED", active_slot=None, ended=2000,
                    end_reason="MODEL_COMPLETED"),
                run("r2", "twice", "ACTIVE", parent="r1", active_slot=1),
            ],
        },
        "steps": [
            {"op": "repairRunBranches", "conversationId": "twice"},
            {"op": "getMaintenanceConversation", "conversationId": "twice"},
            {"op": "repairRunBranches", "conversationId": "twice"},
            {"op": "repairRunBranches", "conversationId": "twice"},
        ],
    })

    write("13-claim-then-repair-mix", {
        "seed": {
            "conversations": [conversation("c1", '{"r1":"r2"}')],
            "runs": [
                run("r1", "c1", "STOPPING", active_slot=1, stopped=1500),
                run("r2", "c1", "STOPPED", parent="r1", active_slot=None, ended=2500,
                    end_reason="USER_STOPPED"),
            ],
        },
        "steps": [
            {"op": "enqueue", "kind": "RUN_BRANCHES", "identity": "c1", "updatedAt": 5},
            {"op": "claim", "claimId": "worker", "claimedAt": 50, "staleBefore": 20,
             "limit": 10},
            {"op": "repairRunBranches", "conversationId": "c1"},
            {"op": "getMaintenanceRuns", "conversationId": "c1"},
            {"op": "complete", "debt": debt("RUN_BRANCHES", "c1", state="CLAIMED", revision=1,
                                            updated=5, claim_id="worker", claimed_at=50)},
            {"op": "hasDebt"},
            {"op": "getClaimCandidates", "staleBefore": 0, "limit": 10},
        ],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
