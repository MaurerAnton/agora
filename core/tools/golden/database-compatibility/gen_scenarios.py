#!/usr/bin/env python3
"""Generates the database-compatibility scenarios. Deterministic; rerun after editing.

`classify` pins the fail-closed inspection of the durable database: a missing database short-
circuits before the reader, a reader exception becomes Unreadable (the sealed interface catches
`Exception`, not `Error`), and only a strictly newer stored version is a FutureVersion. `can_open`
pins which classifications Room may open.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def classify(name, exists, current, stored=None, throw_class=None, throw_message=None):
    fields = {"op": "classify", "databaseExists": exists, "currentVersion": current}
    if stored is not None:
        fields["storedVersion"] = stored
    if throw_class is not None:
        fields["throwClass"] = throw_class
    if throw_message is not None:
        fields["throwMessage"] = throw_message
    case(name, **fields)


def can_open(name, exists, current, stored=None, throw_class=None, throw_message=None):
    fields = {"op": "can_open", "databaseExists": exists, "currentVersion": current}
    if stored is not None:
        fields["storedVersion"] = stored
    if throw_class is not None:
        fields["throwClass"] = throw_class
    if throw_message is not None:
        fields["throwMessage"] = throw_message
    case(name, **fields)


# ---- classify ---------------------------------------------------------------------------------
classify("classify_missing", False, 5, stored=0)
classify("classify_missing_ignores_reader", False, 5,
         throw_class="java.lang.IllegalStateException", throw_message="boom")
classify("classify_supported_older", True, 5, stored=4)
classify("classify_supported_equal", True, 5, stored=5)
classify("classify_supported_zero", True, 0, stored=0)
classify("classify_supported_negative_stored", True, 5, stored=-1)
classify("classify_supported_int_min", True, 0, stored=-2147483648)
classify("classify_future_one_ahead", True, 5, stored=6)
classify("classify_future_far", True, 5, stored=99)
classify("classify_future_negative_current", True, -1, stored=0)
classify("classify_future_int_extremes", True, -2147483648, stored=2147483647)

classify("classify_unreadable_ise", True, 5,
         throw_class="java.lang.IllegalStateException", throw_message="boom")
classify("classify_unreadable_ise_no_message", True, 5,
         throw_class="java.lang.IllegalStateException")
classify("classify_unreadable_ise_empty_message", True, 5,
         throw_class="java.lang.IllegalStateException", throw_message="")
classify("classify_unreadable_npe", True, 5,
         throw_class="java.lang.NullPointerException", throw_message="x is null")
classify("classify_unreadable_runtime", True, 5,
         throw_class="java.lang.RuntimeException", throw_message="db locked")
classify("classify_unreadable_io", True, 5,
         throw_class="java.io.IOException", throw_message="disk")
classify("classify_unreadable_argument", True, 5,
         throw_class="java.lang.IllegalArgumentException", throw_message="version")

# ---- canOpen ----------------------------------------------------------------------------------
can_open("can_open_missing", False, 5, stored=0)
can_open("can_open_supported", True, 5, stored=4)
can_open("can_open_supported_negative_stored", True, 5, stored=-1)
can_open("can_open_future", True, 5, stored=6)
can_open("can_open_unreadable", True, 5,
         throw_class="java.lang.IllegalStateException", throw_message="boom")


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
