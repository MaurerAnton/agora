#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/tz.hpp"

namespace agora::core::cron {

/**
 * Native port of automation/CronExpression.kt: a parsed standard 5-field cron expression
 * `minute hour day-of-month month day-of-week`.
 *
 * Supported per field: `*`, single values, lists (`a,b`), ranges (`a-b`) and steps (a bare
 * star with `/n`, `a-b/n`, and `a/n` meaning a, a+n, ... up to the field max -- note that a
 * literal star-slash-n would end this comment). Day-of-week is 0-6 with
 * 0 = Sunday, and `7` is also accepted as Sunday. When both day fields are restricted a tick
 * matches if EITHER matches -- the standard Vixie-cron rule.
 */
class CronExpression {
   public:
    /** Parses @p expr; nullopt if it is not a well-formed 5-field expression. */
    static std::optional<CronExpression> parse(std::string_view expr);

    /** Whether the expression parses. */
    static bool is_valid(std::string_view expr);

    /**
     * The first instant strictly after @p after_millis that matches, evaluated in @p zone.
     * Nullopt if nothing matches within the search horizon. Seconds and milliseconds of the
     * starting point are truncated before the search advances, so a starting point inside a
     * minute still resolves to the minute after the one it sits in.
     *
     * The walk goes day -> hour -> minute. Whole days and hours whose date or hour field cannot
     * match are skipped, because those fields are constant across the skipped span. That keeps a
     * Feb-29-only schedule at roughly 3k day probes instead of the 4.2M minute steps a naive walk
     * would need, and stepping minute by minute inside a matching hour is what keeps the walk
     * exact across daylight saving gaps and repeats.
     */
    std::optional<std::int64_t> next(std::int64_t after_millis, const tz::Zone& zone) const;

    const std::set<int>& minutes() const { return minutes_; }
    const std::set<int>& hours() const { return hours_; }
    const std::set<int>& days_of_month() const { return days_of_month_; }
    const std::set<int>& months() const { return months_; }
    /**
     * Day-of-week values in INSERTION order, not sorted order.
     *
     * Kotlin builds this field with map{}.toSet(), which yields a LinkedHashSet, while the other
     * four fields come out of a sortedSetOf. The difference is invisible to matching but it is
     * visible in the serialized shape, so the port keeps it.
     */
    const std::vector<int>& days_of_week() const { return days_of_week_; }
    bool dom_restricted() const { return dom_restricted_; }
    bool dow_restricted() const { return dow_restricted_; }

   private:
    CronExpression(std::set<int> minutes, std::set<int> hours, std::set<int> days_of_month,
                   std::set<int> months, std::vector<int> days_of_week, bool dom_restricted,
                   bool dow_restricted)
        : minutes_(std::move(minutes)),
          hours_(std::move(hours)),
          days_of_month_(std::move(days_of_month)),
          months_(std::move(months)),
          days_of_week_(std::move(days_of_week)),
          dom_restricted_(dom_restricted),
          dow_restricted_(dow_restricted) {}

    /** Whether the civil day satisfies the month plus the dom/dow constraints. */
    bool day_matches(const tz::CivilTime& time) const;

    std::set<int> minutes_;
    std::set<int> hours_;
    std::set<int> days_of_month_;
    std::set<int> months_;
    std::vector<int> days_of_week_;
    bool dom_restricted_ = false;
    bool dow_restricted_ = false;
};

}  // namespace agora::core::cron