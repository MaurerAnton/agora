#!/usr/bin/env python3
"""Generates the loop-policy scenarios. Deterministic; rerun after editing.

The interesting inputs are the boundaries and the integer wrap-around, not the happy path:
`nextFireAt` guards overflow with `now > Long.MAX_VALUE - intervalMs`, which itself overflows when
the interval is negative, so a negative interval clamps to MAX for almost every `now`.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

LONG_MAX = 9223372036854775807
LONG_MIN = -9223372036854775808
INT_MAX = 2147483647
INT_MIN = -2147483648
NBSP = "\u00a0"
ZWSP = "\u200b"

ALL = []


def case(name, op, **kw):
    entry = {"name": name, "op": op}
    entry.update(kw)
    ALL.append(entry)


# ---- validate: the interval range -----------------------------------------------------------
case("validate_accepts_defaults", "validate", intervalMs=60_000, maxCycles=10)
case("validate_accepts_min_interval", "validate", intervalMs=10_000, maxCycles=10)
case("validate_accepts_max_interval", "validate", intervalMs=604_800_000, maxCycles=10)
case("validate_rejects_below_min_interval", "validate", intervalMs=9_999, maxCycles=10)
case("validate_rejects_above_max_interval", "validate", intervalMs=604_800_001, maxCycles=10)
case("validate_rejects_zero_interval", "validate", intervalMs=0, maxCycles=10)
case("validate_rejects_negative_interval", "validate", intervalMs=-1, maxCycles=10)
case("validate_rejects_long_min_interval", "validate", intervalMs=LONG_MIN, maxCycles=10)
case("validate_rejects_long_max_interval", "validate", intervalMs=LONG_MAX, maxCycles=10)

# ---- validate: the cycle range --------------------------------------------------------------
case("validate_accepts_min_cycles", "validate", intervalMs=60_000, maxCycles=1)
case("validate_accepts_max_cycles", "validate", intervalMs=60_000, maxCycles=100)
case("validate_rejects_zero_cycles", "validate", intervalMs=60_000, maxCycles=0)
case("validate_rejects_101_cycles", "validate", intervalMs=60_000, maxCycles=101)
case("validate_rejects_negative_cycles", "validate", intervalMs=60_000, maxCycles=-1)
case("validate_rejects_int_min_cycles", "validate", intervalMs=60_000, maxCycles=INT_MIN)
case("validate_rejects_int_max_cycles", "validate", intervalMs=60_000, maxCycles=INT_MAX)

# The interval is checked first, so an input that breaks both rules reports the interval.
case("validate_interval_error_wins", "validate", intervalMs=0, maxCycles=0)
case("validate_interval_error_wins_above_max", "validate", intervalMs=604_800_001, maxCycles=101)

# ---- normalizePrompt -------------------------------------------------------------------------
case("normalize_null", "normalizePrompt", prompt=None)
case("normalize_empty", "normalizePrompt", prompt="")
case("normalize_spaces", "normalizePrompt", prompt="   ")
case("normalize_tabs_and_newlines", "normalizePrompt", prompt="\t\n\r ")
case("normalize_trims", "normalizePrompt", prompt="  hello  ")
case("normalize_keeps_inner_spaces", "normalizePrompt", prompt=" a  b ")
case("normalize_keeps_inner_newline", "normalizePrompt", prompt="a\nb")
case("normalize_keeps_inner_tab_between_words", "normalizePrompt", prompt=" a \t b ")
case("normalize_unicode_text", "normalizePrompt", prompt="  Привет  ")
# Kotlin's trim() uses Char.isWhitespace, which is true for a no-break space but false for a
# zero-width space: one is trimmed away, the other is kept as content.
case("normalize_nbsp_only_collapses", "normalizePrompt", prompt=NBSP)
case("normalize_nbsp_is_trimmed", "normalizePrompt", prompt=NBSP + "x" + NBSP)
case("normalize_zero_width_space_is_content", "normalizePrompt", prompt=ZWSP)

# ---- promptForExecution ----------------------------------------------------------------------
case("execution_null_falls_back", "promptForExecution", prompt=None)
case("execution_blank_falls_back", "promptForExecution", prompt="   ")
case("execution_nbsp_falls_back", "promptForExecution", prompt=NBSP)
case("execution_trims", "promptForExecution", prompt="  keep going  ")

# ---- nextFireAt ------------------------------------------------------------------------------
case("fire_normal", "nextFireAt", now=1_000, intervalMs=60_000)
case("fire_exact_max", "nextFireAt", now=LONG_MAX - 60_000, intervalMs=60_000)
case("fire_clamps_on_overflow", "nextFireAt", now=LONG_MAX - 60_000 + 1, intervalMs=60_000)
case("fire_zero_interval", "nextFireAt", now=123, intervalMs=0)
case("fire_negative_now", "nextFireAt", now=-5, intervalMs=10)
case("fire_max_now_zero_interval", "nextFireAt", now=LONG_MAX, intervalMs=0)
# The guard is `now > Long.MAX_VALUE - intervalMs`. With a negative interval that subtraction
# itself wraps to a large negative number, so everything except Long.MIN clamps to MAX.
case("fire_negative_interval_wraps_threshold", "nextFireAt", now=0, intervalMs=-10)
case("fire_negative_interval_at_long_min_wraps_sum", "nextFireAt", now=LONG_MIN, intervalMs=-10)
case("fire_negative_interval_at_long_max", "nextFireAt", now=LONG_MAX, intervalMs=-10)

# ---- nextRevision ----------------------------------------------------------------------------
case("revision_from_zero", "nextRevision", current=0)
case("revision_from_forty_two", "nextRevision", current=42)
case("revision_from_max_minus_one", "nextRevision", current=LONG_MAX - 1)
case("revision_at_max_wraps_to_zero", "nextRevision", current=LONG_MAX)


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for stale in sorted(os.listdir(SCENARIOS_DIR)):
        if stale.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, stale))
    seen = set()
    for entry in ALL:
        if entry["name"] in seen:
            raise SystemExit(f"duplicate scenario name: {entry['name']}")
        seen.add(entry["name"])
        with open(os.path.join(SCENARIOS_DIR, entry["name"] + ".json"), "w", encoding="utf-8") as handle:
            json.dump(entry, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
