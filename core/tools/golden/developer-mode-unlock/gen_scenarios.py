#!/usr/bin/env python3
"""Generates the developer-mode-unlock scenarios. Deterministic; rerun after editing.

`advance` pins the pre-unlock coercion (negative counts behave like 0, counts >= 6 unlock on the
next tap), the reset-to-zero on success and the feedback threshold at three remaining taps.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def advance(name, current, enabled=False):
    case(name, op="advance", currentTapCount=current, alreadyEnabled=enabled)


def constants(name):
    case(name, op="constants")


# ---- pre-unlock progression -----------------------------------------------------------------
advance("advance_negative", -5)
advance("advance_int_min", -2147483648)
advance("advance_zero", 0)
advance("advance_one", 1)
advance("advance_two", 2)
advance("advance_three", 3)
advance("advance_four", 4)
advance("advance_five", 5)
advance("advance_six_unlocks", 6)
advance("advance_seven_unlocks", 7)
advance("advance_int_max", 2147483647)

# ---- already enabled ------------------------------------------------------------------------
advance("enabled_zero", 0, True)
advance("enabled_three", 3, True)
advance("enabled_six", 6, True)
advance("enabled_negative", -1, True)

constants("constants")


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
