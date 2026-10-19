#include "agora/core/task_schedule.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::schedule {
namespace {

// The two helpers below mirror the ones in cron_expression.cpp; both ports need them and the
// shared home for them has not been carved out yet.

/** Kotlin String.split(Regex("\\s+")): runs of whitespace collapse, empty pieces survive. */
std::vector<std::string> split_whitespace_runs(std::string_view text) {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    };
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at < text.size()) {
        if (space(text[at])) {
            while (at < text.size() && space(text[at])) ++at;
            continue;
        }
        const std::size_t begin = at;
        while (at < text.size() && !space(text[at])) ++at;
        parts.push_back(std::string(text.substr(begin, at - begin)));
    }
    return parts;
}

/** Kotlin String.toIntOrNull(): an optional sign then digits, whole string, within Int range. */
std::optional<int> to_int_or_null(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t at = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        at = 1;
    }
    if (at >= text.size()) return std::nullopt;
    std::int64_t magnitude = 0;
    for (; at < text.size(); ++at) {
        if (text[at] < '0' || text[at] > '9') return std::nullopt;
        magnitude = magnitude * 10 + (text[at] - '0');
        if (magnitude > 2147483648LL) return std::nullopt;
    }
    if (negative) {
        if (magnitude > 2147483648LL) return std::nullopt;
        return static_cast<int>(magnitude == 2147483648LL ? -2147483648LL : -magnitude);
    }
    if (magnitude > 2147483647LL) return std::nullopt;
    return static_cast<int>(magnitude);
}

/** Whether the instance already carries a real calendar date (monthly / yearly / once). */
bool parse_had_date(ScheduleType type) {
    return type == ScheduleType::monthly || type == ScheduleType::yearly || type == ScheduleType::once;
}

std::string join_days(const std::vector<int>& days) {
    std::vector<int> sorted = days;
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (int day : sorted) {
        if (!out.empty()) out += ',';
        out += std::to_string(day);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------------------------

std::int64_t Context::instant_from_fields(int year, int month, int day, int hour, int minute) const {
    // Java's lenient normalization is linear in the calendar fields, so pushing the month into a
    // year-and-month total and letting days_from_civil roll the day over reproduces it exactly:
    // April 31 becomes May 1, and February 29 in a common year becomes March 1.
    std::int64_t total = static_cast<std::int64_t>(year) * 12 + (month - 1);
    const auto normalized_year = static_cast<int>(total / 12);
    const auto normalized_month = static_cast<int>(total % 12) + 1;
    const tz::CivilTime time{normalized_year, normalized_month, day, hour, minute, 0, 0};
    return zone_.instant_from_wall(tz::wall_millis(time));
}

std::int64_t Context::add_day(std::int64_t instant) const {
    return zone_.instant_from_wall(tz::wall_millis(zone_.civil_at(instant)) + 86'400'000LL);
}

std::int64_t Context::add_year(std::int64_t instant) const {
    const tz::CivilTime time = zone_.civil_at(instant);
    return instant_from_fields(static_cast<int>(time.year) + 1, time.month, time.day, time.hour,
                               time.minute);
}

// ---------------------------------------------------------------------------------------------
// Value object
// ---------------------------------------------------------------------------------------------

std::string to_cron(const TaskSchedule& schedule) {
    const std::string at = std::to_string(schedule.minute) + " " + std::to_string(schedule.hour);
    switch (schedule.type) {
        case ScheduleType::once:
            return "";
        case ScheduleType::daily:
            return at + " * * *";
        case ScheduleType::weekly:
            // An empty selection would produce a malformed field, so it means "every day".
            return schedule.days_of_week.empty() ? at + " * * *"
                                                 : at + " * * " + join_days(schedule.days_of_week);
        case ScheduleType::monthly:
            return at + " " + std::to_string(schedule.day_of_month) + " * *";
        case ScheduleType::yearly:
            return at + " " + std::to_string(schedule.day_of_month) + " " +
                   std::to_string(schedule.month) + " *";
    }
    return "";
}

std::optional<std::int64_t> to_run_at(const TaskSchedule& schedule) {
    if (schedule.type != ScheduleType::once || schedule.once_at_millis <= 0) return std::nullopt;
    return schedule.once_at_millis;
}

TaskSchedule with_once_at(const TaskSchedule& schedule, int year, int month, int day,
                          const Context& context) {
    TaskSchedule next = schedule;
    // The stored date is kept exactly as asked for, even when the instant normalizes -- validation
    // rejects a past date instead of the year silently moving underneath the user.
    next.day_of_month = day;
    next.month = month;
    next.once_at_millis = context.instant_from_fields(year, month, day, schedule.hour, schedule.minute);
    return next;
}

TaskSchedule with_time(const TaskSchedule& schedule, int hour, int minute, const Context& context) {
    TaskSchedule next = schedule;
    next.hour = hour;
    next.minute = minute;
    if (schedule.type != ScheduleType::once) return next;
    const std::int64_t base =
        schedule.once_at_millis > 0 ? schedule.once_at_millis : context.now_millis();
    const tz::CivilTime time = context.civil_at(base);
    return with_once_at(next, static_cast<int>(time.year), time.month, time.day, context);
}

TaskSchedule switched_to(const TaskSchedule& schedule, ScheduleType new_type, const Context& context) {
    if (new_type == schedule.type) return schedule;
    const tz::CivilTime today = context.today();

    TaskSchedule seeded = schedule;
    seeded.type = new_type;
    if (seeded.days_of_week.empty()) seeded.days_of_week = {today.weekday};
    const bool had_date = parse_had_date(schedule.type);
    if (!had_date) {
        seeded.day_of_month = today.day;
        seeded.month = today.month;
    }

    if (new_type != ScheduleType::once) {
        seeded.once_at_millis = 0;
        return seeded;
    }

    // ONCE needs a concrete future instant. A date-less type defaults to the next day when today's
    // time has already passed; a type that already carried a date advances one year.
    const TaskSchedule exact =
        with_once_at(seeded, static_cast<int>(today.year), seeded.month, seeded.day_of_month, context);
    if (exact.once_at_millis > context.now_millis()) return exact;
    const std::int64_t advanced = had_date ? context.add_year(exact.once_at_millis)
                                           : context.add_day(exact.once_at_millis);
    const tz::CivilTime next = context.civil_at(advanced);
    return with_once_at(exact, static_cast<int>(next.year), next.month, next.day, context);
}

// ---------------------------------------------------------------------------------------------
// Reading a stored schedule back
// ---------------------------------------------------------------------------------------------

namespace {

/** A single literal value in [min, max]; anything else is not expressible as an "at HH:MM" row. */
std::optional<int> single_cron_value(std::string_view field, int min, int max) {
    const std::optional<int> value = to_int_or_null(field);
    if (!value.has_value() || *value < min || *value > max) return std::nullopt;
    return value;
}

std::optional<std::vector<int>> cron_value_list(std::string_view field, int min, int max) {
    if (field.empty()) return std::nullopt;
    std::vector<int> values;
    std::size_t begin = 0;
    while (true) {
        const std::size_t at = field.find(',', begin);
        const std::string piece = agora::core::json::trim(field.substr(
            begin, at == std::string_view::npos ? std::string_view::npos : at - begin));
        const std::optional<int> value = to_int_or_null(piece);
        if (!value.has_value()) return std::nullopt;
        values.push_back(*value);
        if (at == std::string_view::npos) break;
        begin = at + 1;
    }
    if (values.empty()) return std::nullopt;
    for (int value : values) {
        if (value < min || value > max) return std::nullopt;
    }
    return values;
}

}  // namespace

std::optional<TaskSchedule> parse(std::string_view cron_expr, std::optional<std::int64_t> run_at,
                                  const Context& context) {
    if (run_at.has_value() && *run_at > 0) {
        const tz::CivilTime time = context.civil_at(*run_at);
        TaskSchedule schedule;
        schedule.type = ScheduleType::once;
        schedule.hour = time.hour;
        schedule.minute = time.minute;
        schedule.day_of_month = time.day;
        schedule.month = time.month;
        schedule.once_at_millis = *run_at;
        return schedule;
    }

    const std::vector<std::string> parts = split_whitespace_runs(agora::core::json::trim(cron_expr));
    if (parts.size() != 5) return std::nullopt;

    // Only a single literal minute and hour map to an "at HH:MM" row.
    const std::optional<int> minute = single_cron_value(parts[0], 0, 59);
    if (!minute.has_value()) return std::nullopt;
    const std::optional<int> hour = single_cron_value(parts[1], 0, 23);
    if (!hour.has_value()) return std::nullopt;

    const bool dom_any = parts[2] == "*";
    const bool month_any = parts[3] == "*";
    const bool dow_any = parts[4] == "*";

    TaskSchedule schedule;
    schedule.hour = *hour;
    schedule.minute = *minute;

    if (dom_any && month_any && dow_any) {
        schedule.type = ScheduleType::daily;
        return schedule;
    }
    if (dom_any && month_any) {
        const std::optional<std::vector<int>> days = cron_value_list(parts[4], 0, 7);
        if (!days.has_value()) return std::nullopt;
        schedule.type = ScheduleType::weekly;
        // toSet() yields a LinkedHashSet, so 7 folding onto 0 collapses onto the existing 0.
        for (int day : *days) {
            const int mapped = day == 7 ? 0 : day;
            if (std::find(schedule.days_of_week.begin(), schedule.days_of_week.end(), mapped) ==
                schedule.days_of_week.end()) {
                schedule.days_of_week.push_back(mapped);
            }
        }
        return schedule;
    }
    if (dow_any && month_any) {
        const std::optional<int> day = single_cron_value(parts[2], 1, 31);
        if (!day.has_value()) return std::nullopt;
        schedule.type = ScheduleType::monthly;
        schedule.day_of_month = *day;
        return schedule;
    }
    if (dow_any) {
        const std::optional<int> day = single_cron_value(parts[2], 1, 31);
        if (!day.has_value()) return std::nullopt;
        const std::optional<int> month = single_cron_value(parts[3], 1, 12);
        if (!month.has_value()) return std::nullopt;
        schedule.type = ScheduleType::yearly;
        schedule.day_of_month = *day;
        schedule.month = *month;
        return schedule;
    }
    // Both day-of-month and day-of-week restricted: cron ORs them, which no editor row can
    // express, so the caller must leave the expression alone.
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------------------------

std::string format_on_date(const TaskSchedule& schedule, const Context& context,
                           const MonthNames& month_names) {
    tz::CivilTime time;
    bool show_year = false;
    if (schedule.type == ScheduleType::once && schedule.once_at_millis > 0) {
        time = context.civil_at(schedule.once_at_millis);
        show_year = true;
    } else {
        // A fixed leap year, so February 29 survives instead of normalizing to March 1. The year
        // still has to go through the zone, because Java reads cal.time -- a lenient Calendar that
        // DOES roll an impossible date over: February 30 in 2000 is March 1.
        time = context.civil_at(context.instant_from_fields(2000, schedule.month, schedule.day_of_month, 0, 0));
    }

    if (show_year) {
        const std::string year = std::to_string(time.year);
        const std::string month = time.month < 10 ? "0" + std::to_string(time.month)
                                                   : std::to_string(time.month);
        const std::string day = time.day < 10 ? "0" + std::to_string(time.day)
                                               : std::to_string(time.day);
        return year + "-" + month + "-" + day;
    }
    return month_names(time.month) + " " + std::to_string(time.day);
}

bool has_schedule(std::string_view cron_expr, std::optional<std::int64_t> run_at) {
    return !agora::core::json::is_blank(cron_expr) || (run_at.has_value() && *run_at > 0);
}

}  // namespace agora::core::schedule