#!/usr/bin/env python3
"""Generates Migration16To17 scenarios. Deterministic; rerun after editing.

Each scenario supplies the conversations and legacy message rows the recording database returns.
The frozen trace pins every SQL statement the migration executes (with typed bind arguments) and
any exception raised after the recorded prefix.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, conversations, messages):
    ALL.append(
        {
            "name": name,
            "op": "migrate",
            "conversations": conversations,
            "messages": messages,
        }
    )


def conversation(cid, selections=None):
    fields = {"id": cid}
    if selections is not None:
        fields["selectedBranchesJson"] = selections
    return fields


def msg(cid, mid, parent, participant, status, timestamp):
    return {
        "conversationId": cid,
        "id": mid,
        "parentId": parent,
        "participant": participant,
        "status": status,
        "timestamp": timestamp,
    }


def user(cid, mid, parent, ts, status="SUCCESS"):
    return msg(cid, mid, parent, "USER", status, ts)


def model(cid, mid, parent, ts, status="SUCCESS"):
    return msg(cid, mid, parent, "MODEL", status, ts)


case("m16_empty", [], [])
case("m16_no_messages", [conversation("c1")], [])
case("m16_single_user", [conversation("c1")], [user("c1", "u", None, 10)])
case("m16_user_model", [conversation("c1")], [
    user("c1", "u", None, 10),
    model("c1", "m", "u", 11),
])
case("m16_two_boundaries", [conversation("c1", '{"null":"u2"}')], [
    user("c1", "u1", None, 10),
    model("c1", "m1", "u1", 11),
    user("c1", "u2", "m1", 12),
    model("c1", "m2", "u2", 13),
])
case("m16_two_conversations", [
    conversation("c1", '{"null":"u1"}'),
    conversation("c2"),
], [
    user("c1", "u1", None, 10),
    model("c1", "m1", "u1", 11),
    user("c2", "u2", None, 20),
])
case("m16_orphan_model", [conversation("c1")], [model("c1", "m", "gone", 10)])
case("m16_selection_valid", [conversation("c1", '{"m1":"u2"}')], [
    user("c1", "u1", None, 10),
    model("c1", "m1", "u1", 11),
    user("c1", "u2", "m1", 12),
    model("c1", "m2", "u2", 13),
])
case("m16_selection_null_key", [conversation("c1", '{"null":"u2"}')], [
    user("c1", "u1", None, 10),
    user("c1", "u2", None, 20),
])
case("m16_selection_null_key_first", [conversation("c1", '{"null":"u1"}')], [
    user("c1", "u1", None, 10),
    user("c1", "u2", None, 20),
])
case("m16_selection_invalid_json", [conversation("c1", "not json")], [
    user("c1", "u", None, 10),
])
case("m16_selection_blank", [conversation("c1", "   ")], [
    user("c1", "u", None, 10),
])
case("m16_selection_non_string", [conversation("c1", '{"a":1}')], [
    user("c1", "u", None, 10),
])
case("m16_selection_non_string_known", [conversation("c1", '{"null":1}')], [
    user("c1", "1", None, 10),
    user("c1", "u1", None, 20),
])
case("m16_selection_unknown_id", [conversation("c1", '{"ghost":"u"}')], [
    user("c1", "u", None, 10),
])
case("m16_unknown_enum_values", [conversation("c1")], [
    msg("c1", "u", None, "USER", "SUCCESS", 10),
    msg("c1", "m", "u", "BOGUS", "BOGUS", 11),
])
case("m16_unknown_status_only", [conversation("c1")], [
    user("c1", "u", None, 10),
    msg("c1", "m", "u", "MODEL", "BOGUS", 11),
])
case("m16_blank_conversation_id", [conversation("")], [user("", "u", None, 10)])
case("m16_duplicate_ids", [conversation("c1")], [
    user("c1", "u", None, 10),
    model("c1", "u", "u", 11),
])
case("m16_unvisited_message", [], [user("c1", "u", None, 10)])
case("m16_conversation_order", [
    conversation("c2", '{"null":"u2"}'),
    conversation("c1", '{"null":"u1"}'),
], [
    user("c2", "u2", None, 20),
    user("c1", "u1", None, 10),
])


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for existing in os.listdir(SCENARIOS_DIR):
        if existing.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, existing))
    for scenario in ALL:
        path = os.path.join(SCENARIOS_DIR, scenario["name"] + ".json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(scenario, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
    print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")


if __name__ == "__main__":
    main()
