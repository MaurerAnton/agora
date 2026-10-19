#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::tz {

/** A civil (wall clock) instant: the fields a calendar shows, with no offset attached. */
struct CivilTime {
    std::int64_t year = 1970;   // proleptic Gregorian, may be negative
    int month = 1;              // 1-12
    int day = 1;                // 1-31
    int hour = 0;               // 0-23
    int minute = 0;             // 0-59
    int second = 0;             // 0-59
    int millis = 0;             // 0-999
    int weekday = 0;            // 0 = Sunday, matching cron's day-of-week field
};

/** Wall-clock milliseconds since 1970-01-01T00:00:00, ignoring any offset. */
std::int64_t wall_millis(const CivilTime& time);

/** The inverse of wall_millis; `millis` may be fractional milliseconds. */
CivilTime civil_from_wall_millis(std::int64_t millis);

/**
 * A resolved IANA time zone, read from the system tz database.
 *
 * This exists because automation/CronExpression.kt resolves instants through
 * java.util.Calendar, and the interesting behaviour lives in how Calendar resolves a wall time
 * that is ambiguous or does not exist. Calendar settles both cases on the zone's STANDARD offset,
 * which is why an hour skipped by a spring-forward transition is never reached at all and a
 * repeated hour resolves to standard time rather than to daylight time.
 */
class Zone {
   public:
    /** Mirrors TimeZone.getTimeZone: an unknown id silently yields GMT rather than failing. */
    explicit Zone(std::string id);

    /** The id this zone was constructed with. */
    const std::string& id() const { return id_; }

    /** False when the id could not be resolved, which is how TimeZone.getTimeZone falls to GMT. */
    bool known() const { return !types_.empty(); }

    /** Offset in milliseconds east of UTC in effect at the given instant. */
    std::int64_t offset_at(std::int64_t utc_millis) const;

    /** The wall fields the given instant shows in this zone. */
    CivilTime civil_at(std::int64_t utc_millis) const;

    /**
     * Resolves wall-clock milliseconds to an instant the way java.util.Calendar does: unambiguous
     * times use their own offset, while ambiguous and non-existent times fall back to the zone's
     * standard offset.
     */
    std::int64_t instant_from_wall(std::int64_t wall) const;

   private:
    struct Transition {
        std::int64_t at;         // UTC seconds
        int type;                // index into types_
    };
    struct Type {
        std::int64_t offset;     // milliseconds east of UTC
        bool is_dst = false;
    };

    std::int64_t standard_offset_for(std::int64_t wall) const;
    int active_type(std::int64_t utc_seconds) const;

    std::string id_;
    std::vector<Transition> transitions_;
    std::vector<Type> types_;
    int leading_type_ = 0;
};

/** Resolves once and caches; the parity harness asks for the same handful of zones repeatedly. */
const Zone& zone_for(std::string_view id);

}  // namespace agora::core::tz