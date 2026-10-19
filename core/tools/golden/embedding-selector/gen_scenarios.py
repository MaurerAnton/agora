#!/usr/bin/env python3
"""Generates the scenario corpus for the embedding-selector golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.

Field conventions:
  name        scenario identifier; must equal the file stem
  page_size   selector page bound (constructor argument)
  query       query embedding, a list of floats
  threshold   minimum score for a row to become a candidate
  limit       maximum number of retained candidates
  rows        flat keyset rows; the loader pages them the way the database would
  raw_pages   verbatim pages returned regardless of the cursor (drives the ordering checks)
  loader      keyset (default) | raw | oversized

A row is {id, message_id, dimension, embedding}; dimension = -1 means "the query's size".
Row ids must be >= 1 and strictly increasing within a scenario: the keyset cursor starts at 0 and
the loader drops ids <= cursor, so the selector's own ordering check would fire otherwise.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

# kotlin Float.MAX_VALUE; squaring it overflows to Infinity, which is how the non-finite
# score branch gets reached from JSON (JSON has no NaN literal).
F_MAX = 3.4028235e38

ALL = []


def add(name, **kw):
    out = {"name": name}
    out.update(kw)
    ALL.append(out)


def row(row_id, embedding, message_id="m", dimension=-1):
    return {"id": row_id, "message_id": message_id, "embedding": embedding, "dimension": dimension}


# ---- degenerate inputs (the selector short-circuits before touching the loader) ----------------
add("empty_query", query=[], rows=[row(1, [1.0, 0.0])])
add("non_positive_limit", query=[1.0, 0.0], limit=0, rows=[row(1, [1.0, 0.0])])
add("negative_limit", query=[1.0, 0.0], limit=-3, rows=[row(1, [1.0, 0.0])])
add("empty_rows", query=[1.0, 0.0], rows=[])
add("non_positive_page_size_is_rejected", page_size=0, query=[1.0, 0.0], rows=[row(1, [1.0, 0.0])])

# ---- basic scoring -----------------------------------------------------------------------------
add("identical_vector", query=[1.0, 0.0], threshold=0.5, limit=5,
    rows=[row(1, [1.0, 0.0], "exact")])
add("orthogonal_vector", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [0.0, 1.0], "perp")])
add("opposite_vector", query=[1.0, 0.0], threshold=-2.0, limit=5,
    rows=[row(1, [-1.0, 0.0], "negated")])
add("three_dimensional", query=[1.0, 1.0, 0.0], threshold=0.5, limit=5,
    rows=[row(1, [1.0, 1.0, 0.0], "same"), row(2, [1.0, 0.0, 0.0], "half")])
add("scale_is_irrelevant", query=[2.0, 0.0], threshold=0.99, limit=5,
    rows=[row(1, [50.0, 0.0], "scaled")])

# ---- threshold behaviour -----------------------------------------------------------------------
add("threshold_excludes_row", query=[1.0, 0.0], threshold=0.9, limit=5,
    rows=[row(1, [1.0, 1.0], "below")])
# A score exactly equal to the threshold is NOT retained (the test is strictly greater), but it
# still counts towards best_score because that maximum is taken before the threshold check.
add("threshold_is_exclusive", query=[1.0, 0.0], threshold=0.70710677, limit=5,
    rows=[row(1, [1.0, 1.0], "at")])
add("best_score_counts_sub_threshold_rows", query=[1.0, 0.0], threshold=0.99, limit=5,
    rows=[row(1, [1.0, 1.0], "below"), row(2, [1.0, 0.0], "exact")])

# ---- invalid rows (shape and non-finite scores are skipped, not fatal) --------------------------
add("dimension_mismatch_is_skipped", query=[1.0, 0.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0, 0.0], "short", dimension=2), row(2, [1.0, 0.0, 0.0], "good")])
add("non_positive_dimension_is_skipped", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0, 0.0], "zero", dimension=0), row(2, [1.0, 0.0], "good")])
add("negative_dimension_is_skipped", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0, 0.0], "negative", dimension=-5), row(2, [1.0, 0.0], "good")])
add("byte_length_mismatch_is_skipped", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0], "truncated", dimension=2), row(2, [1.0, 0.0], "good")])
add("all_rows_invalid", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0], "a", dimension=2), row(2, [1.0], "b", dimension=2)])

# ---- float overflow: the JSON-representable route to a non-finite score -------------------------
add("overflowed_query_scores_zero", query=[F_MAX, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0, 0.0], "normal")])
add("overflowed_row_scores_zero", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [F_MAX, 0.0], "huge")])
add("overflowed_row_and_query_score_non_finite", query=[F_MAX, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [F_MAX, 0.0], "huge"), row(2, [1.0, 0.0], "normal")])

# ---- degenerate geometry -----------------------------------------------------------------------
add("zero_row_scores_zero", query=[1.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [0.0, 0.0], "null-vector")])
add("zero_query_scores_zero", query=[0.0, 0.0], threshold=-1.0, limit=5,
    rows=[row(1, [1.0, 0.0], "any")])

# ---- bounded retention and tie-breaking ---------------------------------------------------------
add("limit_keeps_the_best_rows", query=[1.0, 0.0], threshold=-1.0, limit=2,
    rows=[row(1, [1.0, 0.0], "best"), row(2, [1.0, 1.0], "mid"), row(3, [-1.0, 0.0], "worst")])
add("tie_break_prefers_the_lower_row_id", query=[1.0, 0.0], threshold=-1.0, limit=1,
    rows=[row(2, [1.0, 0.0], "earlier"), row(5, [1.0, 0.0], "later")])
add("equal_scores_keep_the_lowest_ids", query=[1.0, 0.0], threshold=-1.0, limit=2,
    rows=[row(7, [1.0, 0.0], "seven"), row(8, [1.0, 0.0], "eight"), row(9, [1.0, 0.0], "nine")])
add("candidates_are_ordered_by_score_then_id", query=[1.0, 0.0], threshold=-1.0, limit=10,
    rows=[row(1, [1.0, 0.0], "one"), row(2, [1.0, 1.0], "half"), row(3, [-1.0, 0.0], "minus"), row(4, [0.0, 1.0], "zero")])
add("max_retained_tracks_the_high_water_mark", query=[1.0, 0.0], threshold=-1.0, limit=3,
    rows=[row(1, [1.0, 0.0], "a"), row(2, [1.0, 0.0], "b"), row(3, [1.0, 0.0], "c"), row(4, [1.0, 0.0], "d")])

# ---- keyset paging -----------------------------------------------------------------------------
add("pages_are_followed_until_one_is_short", query=[1.0, 0.0], threshold=-1.0, limit=10, page_size=2,
    rows=[row(1, [1.0, 0.0], "a"), row(2, [1.0, 0.0], "b"), row(3, [1.0, 0.0], "c")])
add("full_final_page_still_terminates", query=[1.0, 0.0], threshold=-1.0, limit=10, page_size=2,
    rows=[row(1, [1.0, 0.0], "a"), row(2, [1.0, 0.0], "b")])
add("page_size_larger_than_the_catalog", query=[1.0, 0.0], threshold=-1.0, limit=10, page_size=64,
    rows=[row(1, [1.0, 0.0], "a"), row(2, [1.0, 0.0], "b")])

# ---- loader contract violations (the selector's own checks) --------------------------------------
add("unordered_page_is_rejected", query=[1.0, 0.0], threshold=-1.0, limit=5, loader="raw",
    raw_pages=[[row(5, [1.0, 0.0], "five"), row(3, [1.0, 0.0], "three")]])
add("repeated_id_is_rejected", query=[1.0, 0.0], threshold=-1.0, limit=5, loader="raw",
    raw_pages=[[row(4, [1.0, 0.0], "a"), row(4, [1.0, 0.0], "b")]])
# page_size = 1 so the first page is "full" and the cursor advances onto the second, backwards page.
add("page_moving_backwards_is_rejected", query=[1.0, 0.0], threshold=-1.0, limit=5, page_size=1,
    loader="raw", raw_pages=[[row(7, [1.0, 0.0], "seven")], [row(2, [1.0, 0.0], "two")]])
add("oversized_page_is_rejected", query=[1.0, 0.0], threshold=-1.0, limit=5, page_size=2, loader="oversized",
    rows=[row(1, [1.0, 0.0], "a"), row(2, [1.0, 0.0], "b"), row(3, [1.0, 0.0], "c")])


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
