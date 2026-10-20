#!/usr/bin/env python3
"""Generates the context-budget scenarios. Deterministic; rerun after editing.

`normalize` pins the legacy-message-window migration and the [4096, 1048576] clamp; `label` pins
the compact formatting for occupancy values, including the 1000/1024 boundaries and Long math in
compactUnitLabel.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def normalize(name, value):
    case(name, op="normalize", value=value)


def label(name, tokens):
    case(name, op="label", tokens=tokens)


def presets(name):
    case(name, op="presets")


# ---- normalize -------------------------------------------------------------------------------
normalize("normalize_null", None)
normalize("normalize_zero", 0)
normalize("normalize_negative", -1)
normalize("normalize_one", 1)
normalize("normalize_four", 4)
normalize("normalize_fifty", 50)
normalize("normalize_sixty_four", 64)
normalize("normalize_twenty", 20)
normalize("normalize_hundred", 100)
normalize("normalize_above_legacy", 101)
normalize("normalize_thousand", 1000)
normalize("normalize_min", 4095)
normalize("normalize_exact_min", 4096)
normalize("normalize_8192", 8192)
normalize("normalize_200000", 200000)
normalize("normalize_exact_max", 1048576)
normalize("normalize_over_max", 2000000)
normalize("normalize_int_max", 2147483647)

# ---- label -----------------------------------------------------------------------------------
label("label_zero", 0)
label("label_negative", -1)
label("label_negative_large", -1000)
label("label_one", 1)
label("label_999", 999)
label("label_1000", 1000)
label("label_1001", 1001)
label("label_1024", 1024)
label("label_1536", 1536)
label("label_2048", 2048)
label("label_4095", 4095)
label("label_4096", 4096)
label("label_4097", 4097)
label("label_4198", 4198)
label("label_8192", 8192)
label("label_65536", 65536)
label("label_262144", 262144)
label("label_524288", 524288)
label("label_1000000", 1000000)
label("label_1048575", 1048575)
label("label_1048576", 1048576)
label("label_1048577", 1048577)
label("label_1572864", 1572864)
label("label_5242880", 5242880)
label("label_int_max", 2147483647)
label("label_123456789", 123456789)

# ---- presets ---------------------------------------------------------------------------------
presets("presets_list")


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
