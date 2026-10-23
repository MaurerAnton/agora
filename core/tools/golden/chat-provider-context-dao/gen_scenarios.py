#!/usr/bin/env python3
"""Deterministic scenario generator for the chat-provider-context-dao stand.

The dataset targets every declaration of data/local/ChatProviderContextDao.kt: the nullable
provider-state row, the timestamp/id ordering of the topology projection, all seven MessageStatus
and three Participant enum constants, the observed-query Flow, the @Transaction snapshot's
null-state short circuit, and the enum-mapping failures Room surfaces for unknown wire values.
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


def conversation(identifier: str, selected_branches: str | None = None) -> dict:
    return {"id": identifier, "selectedBranchesJson": selected_branches}


def run(conversation_id: str) -> dict:
    return {"id": "run-" + conversation_id, "conversationId": conversation_id,
            "status": "COMPLETED", "startedAt": 100, "lastCheckpointAt": 100,
            "endedAt": 200, "endReason": "MODEL_COMPLETED", "currentPass": 0,
            "legacyAmbiguous": False}


def message(identifier: str, conversation_id: str, *, timestamp: int = 0,
            status: str = "SUCCESS", participant: str = "MODEL",
            token_count: int = 0, parent_id: str | None = None,
            model_name: str | None = None, run_sequence: int = -1,
            consumed_at_pass: int | None = None) -> dict:
    row = {
        "id": identifier,
        "conversationId": conversation_id,
        "status": status,
        "participant": participant,
        "timestamp": timestamp,
        "tokenCount": token_count,
        "runId": "run-" + conversation_id,
        "runSequence": run_sequence,
    }
    if parent_id is not None:
        row["parentId"] = parent_id
    if model_name is not None:
        row["modelName"] = model_name
    if consumed_at_pass is not None:
        row["consumedAtPass"] = consumed_at_pass
    return row


def seed(conversations: list[dict], messages: list[dict] | None = None) -> dict:
    return {
        "conversations": conversations,
        "runs": [run(row["id"]) for row in conversations],
        "messages": messages or [],
    }


def op(name: str, conversation_id: str) -> dict:
    return {"op": name, "conversationId": conversation_id}


def main() -> None:
    write("01-provider-context-state", {
        "seed": seed([
            conversation("c-1", '{"m-1":"m-2"}'),
            conversation("c-2", None),
            conversation("c-3", ""),
            conversation("c-4", "[]"),
        ]),
        "steps": [
            op("getProviderContextState", "c-1"),
            op("getProviderContextState", "c-2"),
            op("getProviderContextState", "c-3"),
            op("getProviderContextState", "c-4"),
            op("getProviderContextState", "missing"),
        ],
    })
    write("02-message-topology", {
        "seed": seed(
            [conversation("c-1"), conversation("c-2")],
            [
                message("m-02", "c-1", timestamp=10, status="THINKING",
                        participant="USER"),
                message("m-03", "c-1", timestamp=10, status="ERROR",
                        participant="ERROR", token_count=7, parent_id="m-02",
                        model_name="o3", run_sequence=0, consumed_at_pass=0),
                message("m-04", "c-1", timestamp=20, status="STOPPED",
                        token_count=3, parent_id="m-03", model_name="gpt-5",
                        run_sequence=1, consumed_at_pass=1),
                message("m-10", "c-1", timestamp=30, status="SUCCESS",
                        token_count=5, parent_id="m-04", model_name="gpt-5",
                        run_sequence=2, consumed_at_pass=2),
                message("m-05", "c-1", timestamp=40, status="SENDING",
                        participant="USER"),
                message("m-06", "c-1", timestamp=50, status="TRANSCRIBING",
                        participant="USER"),
                message("m-07", "c-1", timestamp=60, status="TOOL_CALLING"),
                message("m-01", "c-2", timestamp=5),
            ],
        ),
        "steps": [
            op("getMessageContextTopology", "c-1"),
            op("getMessageContextTopology", "c-2"),
            op("getMessageContextTopology", "missing"),
            op("getMessageContextTopology", ""),
        ],
    })
    write("03-observe-topology", {
        "seed": seed(
            [conversation("c-1"), conversation("c-2")],
            [
                message("m-02", "c-1", timestamp=1, status="STOPPED",
                        participant="USER"),
                message("m-01", "c-1", timestamp=2),
                message("m-03", "c-2", timestamp=0),
            ],
        ),
        "steps": [
            op("observeMessageContextTopology", "c-1"),
            op("observeMessageContextTopology", "c-2"),
            op("observeMessageContextTopology", "missing"),
        ],
    })
    write("04-topology-snapshot", {
        "seed": seed(
            [conversation("c-01", '{"m-1":"m-2"}'), conversation("c-02", None),
             conversation("c-03", ""), conversation("c-04", "[]")],
            [
                message("m-02", "c-01", timestamp=20),
                message("m-01", "c-01", timestamp=10, status="THINKING",
                        participant="USER"),
                message("m-03", "c-02", timestamp=5),
                message("m-04", "c-03", timestamp=6),
            ],
        ),
        "steps": [
            op("getProviderContextTopologySnapshot", "c-01"),
            op("getProviderContextTopologySnapshot", "c-02"),
            op("getProviderContextTopologySnapshot", "c-03"),
            op("getProviderContextTopologySnapshot", "c-04"),
            op("getProviderContextTopologySnapshot", "missing"),
        ],
    })
    write("05-invalid-enums", {
        "seed": seed(
            [conversation("c-1"), conversation("c-2")],
            [
                message("m-01", "c-1", participant="BOGUS"),
                message("m-02", "c-2", status="BOGUS"),
            ],
        ),
        "steps": [
            op("getMessageContextTopology", "c-1"),
            op("getMessageContextTopology", "c-2"),
            op("getProviderContextTopologySnapshot", "c-1"),
        ],
    })


if __name__ == "__main__":
    main()
