#!/usr/bin/env python3
"""Generates the task-schedule scenarios. Deterministic; rerun after editing.

Every scenario pins one operation of TaskSchedule.kt. The driver sets the JVM's default time zone
and locale, so the Calendar and SimpleDateFormat behaviour they drive is deterministic and the
resolved values are recorded in the trace.

Two families are deliberately NOT generated, because TaskSchedule.kt reads the wall clock directly
through `Calendar.getInstance()` and `System.currentTimeMillis()` and a stand cannot freeze either:

  - `switchedTo` when it has to seed days-of-week, day-of-month or month from today, i.e. whenever
    the day selection is empty or the previous type carried no real date (DAILY/WEEKLY sources).
  - `switchedTo(ONCE)` in general, because it compares the candidate instant against now.
  - `withTime` on an ONCE schedule whose onceAtMillis is 0, which reads today for the date.

Those paths are ported in full but are not golden-pinned; the stand README says so explicitly.
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "scenarios")

SCENARIOS_LIST = []


def case(name, op, tz="UTC", locale="en-US", **kwargs):
    entry = {"name": name, "op": op, "tz": tz, "locale": locale}
    entry.update(kwargs)
    SCENARIOS_LIST.append(entry)


def schedule(type_, hour=0, minute=0, days=None, day=1, month=1, once=0):
    out = {"type": type_, "hour": hour, "minute": minute, "dayOfMonth": day, "month": month,
           "onceAtMillis": once}
    if days is not None:
        out["daysOfWeek"] = days
    return out


# ---------------------------------------------------------------------------------------------
# toCron
# ---------------------------------------------------------------------------------------------

case("cron_daily", "toCron", schedule=schedule("DAILY", 9, 0))
case("cron_daily_with_zero_minute", "toCron", schedule=schedule("DAILY", 0, 0))
case("cron_daily_ignores_a_stale_day_selection", "toCron",
     schedule=schedule("DAILY", 9, 0, days=[3, 1]))
case("cron_once_is_empty", "toCron", schedule=schedule("ONCE", 14, 30))
case("cron_weekly", "toCron", schedule=schedule("WEEKLY", 8, 45, days=[1, 3, 5]))
case("cron_weekly_sorts_its_days", "toCron", schedule=schedule("WEEKLY", 8, 45, days=[5, 1, 3]))
case("cron_weekly_with_no_days_falls_back_to_daily", "toCron", schedule=schedule("WEEKLY", 8, 45))
case("cron_weekly_on_sunday", "toCron", schedule=schedule("WEEKLY", 6, 0, days=[0]))
case("cron_weekly_on_saturday", "toCron", schedule=schedule("WEEKLY", 6, 0, days=[6]))
case("cron_monthly", "toCron", schedule=schedule("MONTHLY", 3, 15, day=23))
case("cron_monthly_ignores_its_day_selection", "toCron",
     schedule=schedule("MONTHLY", 3, 15, days=[2], day=23))
case("cron_yearly", "toCron", schedule=schedule("YEARLY", 0, 0, day=29, month=2))
case("cron_daily_keeps_a_two_digit_minute", "toCron", schedule=schedule("DAILY", 13, 45))

# ---------------------------------------------------------------------------------------------
# toRunAt
# ---------------------------------------------------------------------------------------------

case("run_at_only_for_a_positive_one_shot", "toRunAt",
     schedule=schedule("ONCE", 14, 30, day=10, month=6, once=1780000000000))
case("run_at_none_for_a_recurring_schedule", "toRunAt", schedule=schedule("DAILY", 9, 0))
case("run_at_none_for_a_zero_epoch", "toRunAt",
     schedule=schedule("ONCE", 14, 30, day=10, month=6, once=0))
case("run_at_none_for_a_negative_epoch", "toRunAt",
     schedule=schedule("ONCE", 14, 30, day=10, month=6, once=-5))

# ---------------------------------------------------------------------------------------------
# parse
# ---------------------------------------------------------------------------------------------

case("parse_daily", "parse", cronExpr="30 9 * * *")
case("parse_daily_with_surrounding_whitespace", "parse", cronExpr="  30   9  *  *  *  ")
case("parse_weekly_single_day", "parse", cronExpr="0 8 * * 3")
case("parse_weekly_several_days", "parse", cronExpr="45 8 * * 1,3,5")
case("parse_weekly_sunday_as_seven_folds_to_zero", "parse", cronExpr="0 6 * * 7")
case("parse_weekly_with_both_sunday_spellings", "parse", cronExpr="0 6 * * 0,7")
case("parse_weekly_days_are_not_sorted", "parse", cronExpr="0 6 * * 5,1")
case("parse_weekly_with_spaces_after_commas", "parse", cronExpr="0 6 * * 1, 3")
case("parse_monthly", "parse", cronExpr="15 3 23 * *")
case("parse_yearly", "parse", cronExpr="0 0 29 2 *")
case("parse_once_wins_over_a_cron", "parse", cronExpr="30 9 * * *", runAt=1780000000000)
case("parse_once_with_a_zero_run_at_falls_back_to_the_cron", "parse", cronExpr="30 9 * * *", runAt=0)
case("parse_null_run_at_falls_back_to_the_cron", "parse", cronExpr="30 9 * * *", runAt=None)

# Expressions this editor model cannot express must come back untouched, never approximated.
case("parse_rejects_an_hourly_preset", "parse", cronExpr="0 * * * *")
case("parse_rejects_a_step_expression", "parse", cronExpr="*/15 * * * *")
case("parse_rejects_a_range_expression", "parse", cronExpr="0 9-17 * * *")
case("parse_rejects_a_list_of_minutes", "parse", cronExpr="0,30 * * * *")
case("parse_rejects_both_day_fields_restricted", "parse", cronExpr="0 0 13 * 5")
case("parse_rejects_a_restricted_month_with_wildcard_day", "parse", cronExpr="0 0 * 3 *")
case("parse_rejects_four_fields", "parse", cronExpr="30 9 * *")
case("parse_rejects_six_fields", "parse", cronExpr="30 9 * * * *")
case("parse_rejects_a_blank_expression", "parse", cronExpr="")
case("parse_rejects_a_non_numeric_minute", "parse", cronExpr="x 9 * * *")
case("parse_rejects_a_minute_above_max", "parse", cronExpr="60 9 * * *")
case("parse_rejects_a_hour_above_max", "parse", cronExpr="30 24 * * *")
case("parse_rejects_a_day_above_max", "parse", cronExpr="0 0 32 * *")
case("parse_rejects_a_day_below_min", "parse", cronExpr="0 0 0 * *")
case("parse_rejects_a_month_above_max", "parse", cronExpr="0 0 1 13 *")
case("parse_rejects_a_weekday_above_max", "parse", cronExpr="0 0 * * 8")
case("parse_rejects_an_empty_weekly_selection", "parse", cronExpr="0 6 * * ")
case("parse_rejects_a_weekly_day_that_is_not_a_number", "parse", cronExpr="0 6 * * 1,x")
case("parse_rejects_a_step_in_the_day_of_week", "parse", cronExpr="0 6 * * */2")

# ---------------------------------------------------------------------------------------------
# switchedTo -- only the clock-free paths
# ---------------------------------------------------------------------------------------------

case("switched_to_the_same_type_is_a_no_op", "switchedTo",
     schedule=schedule("MONTHLY", 3, 15, day=23), newType="MONTHLY")
case("switched_monthly_to_daily_keeps_the_date_fields", "switchedTo",
     schedule=schedule("MONTHLY", 3, 15, days=[2, 4], day=23, month=11), newType="DAILY")
case("switched_monthly_to_weekly_keeps_the_day_selection", "switchedTo",
     schedule=schedule("MONTHLY", 3, 15, days=[4, 2], day=23), newType="WEEKLY")
case("switched_yearly_to_weekly_keeps_the_day_selection", "switchedTo",
     schedule=schedule("YEARLY", 0, 0, days=[1], day=29, month=2), newType="WEEKLY")
case("switched_once_to_daily_clears_the_one_shot_epoch", "switchedTo",
     schedule=schedule("ONCE", 14, 30, days=[3], day=10, month=6, once=1780000000000),
     newType="DAILY")
case("switched_once_to_weekly_clears_the_one_shot_epoch", "switchedTo",
     schedule=schedule("ONCE", 14, 30, days=[3], day=10, month=6, once=1780000000000),
     newType="WEEKLY")

# ---------------------------------------------------------------------------------------------
# withOnceAt
# ---------------------------------------------------------------------------------------------

case("once_at_builds_an_exact_instant", "withOnceAt",
     schedule=schedule("ONCE", 9, 30), year=2027, month=5, day=10, tz="UTC")
case("once_at_in_a_standard_time_zone", "withOnceAt",
     schedule=schedule("ONCE", 9, 30), year=2027, month=1, day=10, tz="Europe/Berlin")
case("once_at_in_a_summer_time_zone", "withOnceAt",
     schedule=schedule("ONCE", 9, 30), year=2027, month=7, day=10, tz="Europe/Berlin")
case("once_at_in_a_half_hour_zone", "withOnceAt",
     schedule=schedule("ONCE", 9, 30), year=2027, month=7, day=10, tz="Asia/Kolkata")
case("once_at_feb_29_in_a_leap_year", "withOnceAt",
     schedule=schedule("ONCE", 0, 0), year=2028, month=2, day=29)
case("once_at_feb_29_in_a_common_year_normalizes", "withOnceAt",
     schedule=schedule("ONCE", 0, 0), year=2027, month=2, day=29)
case("once_at_a_day_past_the_end_of_the_month_normalizes", "withOnceAt",
     schedule=schedule("ONCE", 0, 0), year=2027, month=4, day=31)
case("once_at_dec_31", "withOnceAt", schedule=schedule("ONCE", 23, 59), year=2027, month=12, day=31)
case("once_at_rewrites_the_stored_date", "withOnceAt",
     schedule=schedule("MONTHLY", 9, 30, day=2, month=2), year=2030, month=8, day=21)

# ---------------------------------------------------------------------------------------------
# withTime
# ---------------------------------------------------------------------------------------------

case("with_time_on_a_recurring_schedule_keeps_the_date", "withTime",
     schedule=schedule("MONTHLY", 3, 15, day=23, month=11), newHour=7, newMinute=5)
case("with_time_on_a_daily_schedule", "withTime",
     schedule=schedule("DAILY", 9, 0), newHour=23, newMinute=59)
case("with_time_on_a_one_shot_rebuilds_the_instant", "withTime",
     schedule=schedule("ONCE", 14, 30, day=10, month=6, once=1780000000000), newHour=7, newMinute=5)
case("with_time_on_a_one_shot_keeps_its_date", "withTime",
     schedule=schedule("ONCE", 14, 30, day=10, month=6, once=1780000000000), newHour=9, newMinute=0)
case("with_time_across_a_dst_change_moves_the_instant", "withTime",
     schedule=schedule("ONCE", 14, 30, day=28, month=3, once=1775000000000), newHour=3, newMinute=0,
     tz="Europe/Berlin")

# ---------------------------------------------------------------------------------------------
# formatOnDate
# ---------------------------------------------------------------------------------------------

case("format_on_date_daily_is_yearless", "formatOnDate", schedule=schedule("DAILY", 9, 0, day=14, month=3))
case("format_on_date_monthly_is_yearless", "formatOnDate",
     schedule=schedule("MONTHLY", 9, 0, day=23, month=11))
case("format_on_date_yearly_omits_the_year", "formatOnDate",
     schedule=schedule("YEARLY", 9, 0, day=29, month=2))
case("format_on_date_once_shows_the_full_date", "formatOnDate",
     schedule=schedule("ONCE", 9, 0, day=10, month=6, once=1780000000000), tz="UTC")
case("format_on_date_once_in_another_zone", "formatOnDate",
     schedule=schedule("ONCE", 9, 0, day=10, month=6, once=1780000000000), tz="Asia/Kolkata")
case("format_on_date_feb_29_uses_a_leap_year", "formatOnDate",
     schedule=schedule("MONTHLY", 9, 0, day=29, month=2))
case("format_on_date_the_first_of_january", "formatOnDate",
     schedule=schedule("MONTHLY", 9, 0, day=1, month=1))
# The fixed display year is 2000, a leap year, so February 29 survives -- but an impossible date
# still rolls over, because the Kotlin reads cal.time out of a lenient Calendar.
case("format_on_date_feb_30_still_normalizes", "formatOnDate",
     schedule=schedule("MONTHLY", 9, 0, day=30, month=2))
case("format_on_date_april_31_still_normalizes", "formatOnDate",
     schedule=schedule("MONTHLY", 9, 0, day=31, month=4))
case("format_on_date_the_last_day_of_february", "formatOnDate",
     schedule=schedule("MONTHLY", 9, 0, day=28, month=2))
case("format_on_date_in_german", "formatOnDate", schedule=schedule("MONTHLY", 9, 0, day=14, month=3),
     locale="de-DE")
case("format_on_date_in_french", "formatOnDate",
     schedule=schedule("YEARLY", 9, 0, day=14, month=3), locale="fr-FR")

# ---------------------------------------------------------------------------------------------
# hasSchedule
# ---------------------------------------------------------------------------------------------

case("has_schedule_for_a_cron", "hasSchedule", cronExpr="30 9 * * *")
case("has_schedule_for_a_one_shot", "hasSchedule", cronExpr="", runAt=1780000000000)
case("has_schedule_needs_a_positive_epoch", "hasSchedule", cronExpr="", runAt=0)
case("has_schedule_rejects_a_negative_epoch", "hasSchedule", cronExpr="", runAt=-1)
case("has_schedule_false_when_nothing_is_set", "hasSchedule", cronExpr="   ", runAt=None)
case("has_schedule_blank_cron_with_a_valid_one_shot", "hasSchedule", cronExpr="  ", runAt=1780000000000)


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