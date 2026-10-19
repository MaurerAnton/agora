#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/tz.hpp"

namespace agora::core::schedule {

/** Port of automation/TaskSchedule.kt's ScheduleType. */
enum class ScheduleType { once, daily, weekly, monthly, yearly };

/**
 * Port of automation/TaskSchedule.kt's TaskSchedule.
 *
 * Storage is a 5-field cron for every recurring type. ONCE cannot be a cron -- a 5-field
 * expression has no year, so "March 3rd at 09:00" would silently mean every March 3rd -- so a
 * one-shot carries an absolute epoch in once_at_millis.
 *
 * days_of_week keeps insertion order rather than sorting, because the Kotlin field is a
 * LinkedHashSet built by parse() and switching types; to_cron sorts explicitly, which is a
 * difference the golden traces pin.
 */
struct TaskSchedule {
    ScheduleType type = ScheduleType::daily;
    int hour = 0;
    int minute = 0;
    std::vector<int> days_of_week;
    int day_of_month = 1;
    int month = 1;
    std::int64_t once_at_millis = 0;
};

/**
 * Everything the Kotlin code reads from the environment.
 *
 * TaskSchedule.kt calls `Calendar.getInstance()` and `System.currentTimeMillis()` directly, which
 * makes those results invisible to a test and impossible to freeze in a stand. The port takes them
 * as an explicit context instead: the caller supplies the zone and the current instant, the
 * behaviour is unchanged, and the answer stops depending on hidden globals.
 */
class Context {
   public:
    Context(const tz::Zone& zone, std::int64_t now_millis) : zone_(zone), now_millis_(now_millis) {}

    const tz::Zone& zone() const { return zone_; }
    std::int64_t now_millis() const { return now_millis_; }
    tz::CivilTime today() const { return zone_.civil_at(now_millis_); }
    tz::CivilTime civil_at(std::int64_t instant) const { return zone_.civil_at(instant); }

    /** Java's lenient field arithmetic: an out-of-range day rolls into the following month. */
    std::int64_t instant_from_fields(int year, int month, int day, int hour, int minute) const;

    /** Calendar.add(DAY_OF_MONTH, 1): wall-time day arithmetic, then the offset is re-resolved. */
    std::int64_t add_day(std::int64_t instant) const;
    /** Calendar.add(YEAR, 1): keeps the month and day, so Feb 29 becomes Mar 1 in a common year. */
    std::int64_t add_year(std::int64_t instant) const;

   private:
    const tz::Zone& zone_;
    std::int64_t now_millis_;
};

/** The cron this schedule runs on, or "" for a one-shot. */
std::string to_cron(const TaskSchedule& schedule);

/** The one-shot epoch, or nullopt for a recurring schedule. */
std::optional<std::int64_t> to_run_at(const TaskSchedule& schedule);

/**
 * Switches recurrence while keeping everything the new type can still use. Fields the old type did
 * not have are seeded from today rather than from a default the user never chose.
 */
TaskSchedule switched_to(const TaskSchedule& schedule, ScheduleType new_type, const Context& context);

/** Rebuilds the one-shot instant from an exact calendar date plus the schedule's time of day. */
TaskSchedule with_once_at(const TaskSchedule& schedule, int year, int month, int day,
                          const Context& context);

/** Applies a new time of day, re-deriving the one-shot instant when this is a ONCE schedule. */
TaskSchedule with_time(const TaskSchedule& schedule, int hour, int minute, const Context& context);

/**
 * Reads a stored schedule back into the editor model, or nullopt when the cron is a valid
 * expression this model cannot express -- the legacy hourly preset, a step expression, or both day
 * fields restricted. Callers must leave such expressions alone rather than rewriting them.
 */
std::optional<TaskSchedule> parse(std::string_view cron_expr, std::optional<std::int64_t> run_at,
                                  const Context& context);

/**
 * Supplies the abbreviated month name for a 1-based month. Locale data deliberately lives outside
 * the core: the Kotlin original hands this to SimpleDateFormat with Locale.getDefault(), which is a
 * presentation concern the caller owns.
 */
using MonthNames = std::function<std::string(int month_1_based)>;

/**
 * The "On" row value for the date-bearing types. ONCE shows the full date; YEARLY omits the year,
 * which cron cannot store anyway. The date-less types render against a fixed leap year, because
 * formatting February 29 against the current year would normalize it to March 1.
 */
std::string format_on_date(const TaskSchedule& schedule, const Context& context,
                           const MonthNames& month_names);

/** TaskEntity.hasSchedule(): a recurring cron OR a positive one-shot instant. */
bool has_schedule(std::string_view cron_expr, std::optional<std::int64_t> run_at);

}  // namespace agora::core::schedule