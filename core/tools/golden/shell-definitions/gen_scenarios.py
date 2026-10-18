#!/usr/bin/env python3
"""Generates the scenario corpus for the shell-definitions golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, devices=(), sandbox=False, shell_enabled=True, tool_timeout_ms=60_000):
    ALL.append({
        "name": name,
        "mode": "definitions",
        "shellEnabled": shell_enabled,
        "sandboxEnabled": sandbox,
        "toolTimeoutMs": tool_timeout_ms,
        "devices": [{"name": device[0], "type": device[1]} for device in devices],
    })


# ---- gating -------------------------------------------------------------------------------
case("definitions_shell_disabled", devices=[("box", "conch")], sandbox=True, shell_enabled=False)
case("definitions_no_devices_no_sandbox", devices=[])
case("definitions_no_devices_with_sandbox", devices=[], sandbox=True)

# ---- single / multiple servers ------------------------------------------------------------
case("definitions_single_conch", devices=[("box", "conch")])
case("definitions_single_ssh", devices=[("edge", "ssh")])
case("definitions_multi_mixed", devices=[("box", "conch"), ("edge", "ssh")])
case("definitions_multi_conch", devices=[("box", "conch"), ("farm", "conch")])
case("definitions_sandbox_plus_device", devices=[("box", "conch")], sandbox=True)
case("definitions_ssh_only_plus_sandbox", devices=[("edge", "ssh")], sandbox=True)

# ---- naming edge cases ---------------------------------------------------------------------
case("definitions_blank_device_name", devices=[("   ", "conch")])
case("definitions_quoted_name_with_comma", devices=[('a,b "c"', "conch")])
case("definitions_unknown_device_type", devices=[("mystery", "telepathy")])

# ---- wait_for_job ceiling (maxWaitMs) --------------------------------------------------------
case("definitions_timeout_default", devices=[("box", "conch")], tool_timeout_ms=60_000)
case("definitions_timeout_at_margin", devices=[("box", "conch")], tool_timeout_ms=5_000)
case("definitions_timeout_below_margin", devices=[("box", "conch")], tool_timeout_ms=1_000)
case("definitions_timeout_just_above_margin", devices=[("box", "conch")], tool_timeout_ms=5_001)
case("definitions_timeout_huge", devices=[("box", "conch")], tool_timeout_ms=2_147_483_647)
case("definitions_timeout_negative", devices=[("box", "conch")], tool_timeout_ms=-5)


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()