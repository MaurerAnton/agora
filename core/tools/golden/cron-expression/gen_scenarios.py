#!/usr/bin/env python3
"""Generates the cron-expression scenarios. Deterministic; rerun after editing.

Two families:

  parse  -- the field grammar. Validity alone would hide almost everything, so each trace also
            dumps the five expanded value sets and the two restriction flags.
  next   -- the resolved instant, which is where java.util.Calendar's lenient field arithmetic and
            the timezone database decide the answer. The DST cases are the interesting ones: the
            wall-clock time either does not exist (spring forward) or exists twice (fall back).

The next family stays inside dates where every published tzdata agrees on the US and EU rules,
so the JVM and the C++ port cannot legitimately disagree.
"""

import json
import os
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "scenarios")

EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)
SCENARIOS_LIST = []


def ms(iso):
    """Exact epoch milliseconds for an ISO-8601 UTC instant."""
    moment = datetime.fromisoformat(iso).replace(tzinfo=timezone.utc)
    delta = moment - EPOCH
    return delta.days * 86_400_000 + delta.seconds * 1000 + delta.microseconds // 1000


def parse_case(name, expr):
    SCENARIOS_LIST.append({"name": name, "expr": expr})


def next_case(name, expr, after, zone="UTC"):
    SCENARIOS_LIST.append(
        {"name": name, "expr": expr, "next": True, "after_millis": ms(after), "zone": zone}
    )


# ---------------------------------------------------------------------------------------------
# The field grammar
# ---------------------------------------------------------------------------------------------

parse_case("wildcards_expand_to_every_value", "* * * * *")
parse_case("single_values", "0 12 1 6 *")
parse_case("step_on_wildcard", "*/15 * * * *")
parse_case("step_that_is_wider_than_the_range", "*/60 * * * *")
parse_case("step_of_one_is_the_identity", "*/1 * * * *")
parse_case("range_with_a_step", "0-30/10 * * * *")
parse_case("bare_value_with_a_step_runs_to_the_field_max", "5/10 * * * *")
parse_case("bare_value_without_a_step_is_a_single_value", "5 * * * *")
parse_case("list", "1,2,3 * * * *")
parse_case("list_with_a_step", "1,3,5/2 * * * *")
parse_case("overlapping_list_entries_union", "1,2,1 * * * *")
parse_case("leading_plus_is_accepted_by_to_int_or_null", "+5/10 * * * *")
parse_case("leading_zeros_are_accepted", "05 * * * *")
parse_case("dom_range_with_a_step", "0 0 1-7/2 * *")
parse_case("dom_range_that_ends_on_the_max", "0 0 28-31 * *")
parse_case("month_range", "0 0 1 1-6 *")
parse_case("month_range_with_a_step", "0 0 1 1-12/11 *")
parse_case("leap_day_schedule", "0 0 29 2 *")
parse_case("sunday_as_zero", "0 0 * * 0")
parse_case("sunday_as_seven", "0 0 * * 7")
parse_case("sunday_as_both_collapses_to_one_value", "0 0 * * 0,7")
parse_case("sunday_as_seven_with_a_step", "0 0 * * 7/2")
parse_case("weekday_range_with_a_step", "0 0 * * 1-7/3")
parse_case("weekday_range_with_an_even_step_never_reaches_seven", "0 0 * * 0-7/2")
parse_case("weekday_range_with_a_wide_step", "0 0 * * 1-5/2")
parse_case("weekday_step_from_six", "0 0 * * 6/2")
parse_case("restricted_dom_and_dow", "0 0 13 * 5")
parse_case("restricted_dom_only", "0 0 13 * *")
parse_case("restricted_dow_only", "0 0 * * 5")
parse_case("wildcard_with_a_step_is_restricted", "0 0 */2 * */2")
parse_case("extra_internal_whitespace_collapses", "  \t 0   12  *  *  * \n ")
parse_case("newlines_and_tabs_are_whitespace", "0\t12\n*\r*\v*")

# Rejected forms.
parse_case("empty_expression", "")
parse_case("whitespace_only", "     ")
parse_case("too_few_fields", "* * * *")
parse_case("too_many_fields", "* * * * * *")
parse_case("minute_above_max", "60 * * * *")
parse_case("minute_below_min", "-1 * * * *")
parse_case("hour_above_max", "0 24 * * *")
parse_case("dom_below_min", "0 0 0 * *")
parse_case("dom_above_max", "0 0 32 * *")
parse_case("month_below_min", "0 0 * 0 *")
parse_case("month_above_max", "0 0 * 13 *")
parse_case("dow_above_max", "0 0 * * 8")
parse_case("non_numeric_value", "x * * * *")
parse_case("step_of_zero", "*/0 * * * *")
parse_case("negative_step", "*/-1 * * * *")
parse_case("non_numeric_step", "*/x * * * *")
parse_case("step_with_a_space", "*/ 5 * * * *")
parse_case("two_slashes", "1/2/3 * * * *")
parse_case("empty_list_element", "1,,2 * * * *")
parse_case("trailing_comma", "1, * * * *")
parse_case("leading_comma", ",1 * * * *")
parse_case("three_part_range", "1-2-3 * * * *")
parse_case("range_with_a_missing_start", "-5 * * * *")
parse_case("range_with_a_missing_end", "1- * * * *")
parse_case("range_whose_start_is_above_its_end", "10-5 * * * *")
parse_case("stepped_value_whose_start_is_above_its_end", "10/2 * * * *")
parse_case("bare_minus_sign", "- * * * *")
parse_case("asterisk_with_a_bogus_step", "*/ * * * *")
parse_case("non_breaking_space_is_not_whitespace_to_the_cron_splitter", "1 2 3 4 5")

# ---------------------------------------------------------------------------------------------
# Resolution in UTC
# ---------------------------------------------------------------------------------------------

next_case("every_minute_from_an_exact_minute", "* * * * *", "2026-03-14T12:00:00.000")
next_case("every_minute_from_a_nonzero_second", "* * * * *", "2026-03-14T12:00:30.000")
next_case("every_minute_from_nonzero_millis", "* * * * *", "2026-03-14T12:00:00.500")
next_case("every_minute_from_a_nonzero_second_and_millis", "* * * * *", "2026-03-14T12:00:30.500")
# Seconds are zeroed BEFORE the minute is added, so 12:29:59.999 truncates down to 12:29:00
# and then advances to 12:30:00 -- the match still lands on the same day it would without the
# zeroing, but only because the truncation moves backwards rather than forwards.
next_case("zeroing_seconds_moves_backwards_before_advancing", "30 12 * * *", "2026-03-14T12:29:59.999")
next_case("a_second_past_the_match_moves_to_the_next_day", "30 12 * * *", "2026-03-14T12:30:00.001")
next_case("every_quarter_hour", "*/15 * * * *", "2026-03-14T12:07:00.000")
next_case("top_of_the_hour", "0 * * * *", "2026-03-14T12:30:00.000")
next_case("midnight_from_mid_afternoon", "0 0 * * *", "2026-03-14T12:34:56.789")
next_case("midnight_from_midnight_moves_to_the_next_day", "0 0 * * *", "2026-03-14T00:00:00.000")
next_case("a_moment_that_just_passed_moves_strictly_after", "0 12 * * *", "2026-03-14T12:00:00.000")
next_case("the_same_day_still_matches_later", "0 12 * * *", "2026-03-14T12:00:00.001")
next_case("year_end_wrap", "0 0 1 1 *", "2026-06-01T00:00:00.000")
next_case("leap_day_is_four_years_out", "0 0 29 2 *", "2026-01-01T00:00:00.000")
next_case("leap_day_just_passed_waits_for_the_next_one", "0 0 29 2 *", "2028-02-29T00:00:00.001")
next_case("leap_day_exactly_now_moves_on", "0 0 29 2 *", "2028-02-29T00:00:00.000")
next_case("non_leap_year_rejects_the_29th", "0 0 29 2 *", "2026-02-28T12:00:00.000")
next_case("sunday_matches_zero", "0 0 * * 0", "2026-03-14T12:00:00.000")
next_case("sunday_matches_seven", "0 0 * * 7", "2026-03-14T12:00:00.000")
next_case("weekday_monday_to_friday", "0 9 * * 1-5", "2026-03-14T12:00:00.000")

# The Vixie rule: with both day fields restricted, a tick matches if EITHER does.
next_case("vixie_or_matches_a_friday_before_the_thirteenth", "0 0 13 * 5", "2026-03-01T00:00:00.000")
next_case("vixie_or_matches_the_thirteenth_before_any_friday", "0 0 13 * 5", "2026-04-01T00:00:00.000")
next_case("dom_only_requires_the_day", "0 0 13 * *", "2026-03-01T00:00:00.000")
next_case("dow_only_requires_the_weekday", "0 0 * * 5", "2026-03-01T00:00:00.000")
next_case("vixie_or_with_a_weekend_day", "0 0 1 * 0", "2026-03-02T00:00:00.000")

# ---------------------------------------------------------------------------------------------
# Fixed-offset zones
# ---------------------------------------------------------------------------------------------

next_case("half_hour_offset_zone", "0 12 * * *", "2026-03-14T00:00:00.000", "GMT+05:30")
next_case("negative_eight_offset_zone", "0 0 * * *", "2026-03-14T00:00:00.000", "Etc/GMT+8")
next_case("positive_twelve_offset_zone", "0 0 * * *", "2026-03-14T00:00:00.000", "Etc/GMT-12")
next_case("unknown_zone_falls_back_to_gmt", "0 0 * * *", "2026-03-14T12:00:00.000", "Not/AZone")
next_case("unknown_zone_every_minute", "* * * * *", "2026-03-14T12:00:30.000", "Not/AZone")

# ---------------------------------------------------------------------------------------------
# Daylight saving transitions
# ---------------------------------------------------------------------------------------------

# Europe/Berlin springs forward on 2026-03-29 (02:00 CET -> 03:00 CEST) and falls back on
# 2026-10-25 (03:00 CEST -> 02:00 CET), so 02:30 local is missing then and ambiguous then.
next_case("berlin_spring_forward_the_missing_hour_is_never_reached", "30 2 * * *", "2026-03-28T12:00:00.000", "Europe/Berlin")
next_case("berlin_spring_forward_every_minute", "* * * * *", "2026-03-29T00:59:30.000", "Europe/Berlin")
next_case("berlin_spring_forward_the_hour_that_replaces_it", "0 3 * * *", "2026-03-28T12:00:00.000", "Europe/Berlin")
next_case("berlin_fall_back_resolves_the_repeated_hour_to_standard_time", "30 2 * * *", "2026-10-24T12:00:00.000", "Europe/Berlin")
next_case("berlin_fall_back_every_minute", "* * * * *", "2026-10-25T00:59:30.000", "Europe/Berlin")
next_case("berlin_the_hour_before_the_change", "0 2 * * *", "2026-10-24T12:00:00.000", "Europe/Berlin")
next_case("berlin_a_normal_day_in_standard_time", "0 12 * * *", "2026-01-15T00:00:00.000", "Europe/Berlin")
next_case("berlin_a_normal_day_in_summer_time", "0 12 * * *", "2026-07-15T00:00:00.000", "Europe/Berlin")

# America/New_York springs forward on 2026-03-08 and falls back on 2026-11-01.
next_case("new_york_spring_forward_the_missing_hour_is_never_reached", "30 2 * * *", "2026-03-07T12:00:00.000", "America/New_York")
next_case("new_york_spring_forward_every_minute", "* * * * *", "2026-03-08T06:59:30.000", "America/New_York")
next_case("new_york_fall_back_resolves_the_repeated_hour_to_standard_time", "30 1 * * *", "2026-10-31T12:00:00.000", "America/New_York")
next_case("new_york_fall_back_every_minute", "* * * * *", "2026-11-01T05:59:30.000", "America/New_York")
next_case("new_york_half_hour_offset_in_standard_time", "0 9 * * 1-5", "2026-01-15T00:00:00.000", "America/New_York")

# Southern-hemisphere zones run their rules the other way round.
next_case("sydney_southern_autumn", "0 2 * * *", "2026-04-04T12:00:00.000", "Australia/Sydney")
next_case("sydney_southern_spring_forward_the_missing_hour_is_never_reached", "30 2 * * *", "2026-10-03T12:00:00.000", "Australia/Sydney")

# Zones with no daylight saving at all.
next_case("utc_has_no_transitions", "30 2 * * *", "2026-03-29T00:00:00.000", "UTC")
next_case("asia_colombo_is_a_fixed_offset", "0 12 * * *", "2026-03-14T00:00:00.000", "Asia/Colombo")
next_case("asia_kolkata_half_hour_offset", "0 12 * * *", "2026-03-14T00:00:00.000", "Asia/Kolkata")

# A schedule whose only match is inside a repeated hour, far enough out to walk many days.
next_case("a_weekday_schedule_crosses_a_transition", "0 3 * * 1", "2026-03-02T00:00:00.000", "Europe/Berlin")


def main():
    os.makedirs(SCENARIOS, exist_ok=True)
    for stale in sorted(os.listdir(SCENARIOS)):
        if stale.endswith(".json"):
            os.remove(os.path.join(SCENARIOS, stale))

    seen = set()
    for entry in SCENARIOS_LIST:
        name = entry["name"]
        if name in seen:
            raise SystemExit(f"duplicate scenario name: {name}")
        seen.add(name)
        with open(os.path.join(SCENARIOS, f"{name}.json"), "w", encoding="utf-8") as handle:
            json.dump(entry, handle, indent=2, ensure_ascii=False, sort_keys=True)
            handle.write("\n")
    print(f"wrote {len(SCENARIOS_LIST)} scenarios to {SCENARIOS}")


if __name__ == "__main__":
    main()