#include "agora/core/cron_expression.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace agora::core::cron {
namespace {

// 8 years expressed in wall time; a valid expression always resolves far sooner.
constexpr std::int64_t kHorizonMillis = 366LL * 24 * 60 * 8 * 60'000LL;

/**
 * Kotlin's Regex("\\s+") character class. NBSP is deliberately absent, which is why an
 * expression separated by one has four fields rather than five and fails to parse.
 */
bool is_regex_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/** Kotlin String.trim(): drops Kotlin-whitespace from both ends. */
std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_regex_whitespace(text[begin])) ++begin;
    while (end > begin && is_regex_whitespace(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

/** Kotlin String.split(separator: Char): keeps empty pieces, so "1,,2" yields three. */
std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t at = text.find(separator, begin);
        if (at == std::string_view::npos) {
            parts.push_back(std::string(text.substr(begin)));
            return parts;
        }
        parts.push_back(std::string(text.substr(begin, at - begin)));
        begin = at + 1;
    }
}

/** Kotlin String.split(Regex("\\s+")): runs of whitespace collapse, but empty pieces survive. */
std::vector<std::string> split_whitespace_runs(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at < text.size()) {
        if (is_regex_whitespace(text[at])) {
            while (at < text.size() && is_regex_whitespace(text[at])) ++at;
            continue;
        }
        const std::size_t begin = at;
        while (at < text.size() && !is_regex_whitespace(text[at])) ++at;
        parts.push_back(std::string(text.substr(begin, at - begin)));
    }
    return parts;
}

/**
 * Kotlin's String.toIntOrNull(): an optional sign then digits, the whole string, within Int
 * range. Notably it accepts a leading '+', so "+5/10" is a valid minute field.
 */
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

/**
 * Expands one field to its value set: a star, a single value, a list, a range, or any of those
 * with a "/n" step (a bare value with a step runs up to the field max). Nullopt stands in for the
 * Kotlin `return null`.
 */
std::optional<std::set<int>> parse_field(std::string_view field, int minimum, int maximum) {
    std::set<int> result;
    for (const std::string& token : split(field, ',')) {
        if (token.empty()) return std::nullopt;

        const std::vector<std::string> by_slash = split(token, '/');
        std::string range_part;
        std::optional<std::string> step_part;
        if (by_slash.size() == 1) {
            range_part = by_slash[0];
        } else if (by_slash.size() == 2) {
            range_part = by_slash[0];
            step_part = by_slash[1];
        } else {
            return std::nullopt;
        }

        int step = 1;
        if (step_part.has_value()) {
            const std::optional<int> parsed = to_int_or_null(*step_part);
            if (!parsed.has_value() || *parsed <= 0) return std::nullopt;
            step = *parsed;
        }

        int start = 0;
        int end = 0;
        if (range_part == "*") {
            start = minimum;
            end = maximum;
        } else if (range_part.find('-') != std::string::npos) {
            const std::vector<std::string> bounds = split(range_part, '-');
            if (bounds.size() != 2) return std::nullopt;
            const std::optional<int> low = to_int_or_null(bounds[0]);
            const std::optional<int> high = to_int_or_null(bounds[1]);
            if (!low.has_value() || !high.has_value()) return std::nullopt;
            start = *low;
            end = *high;
        } else {
            const std::optional<int> value = to_int_or_null(range_part);
            if (!value.has_value()) return std::nullopt;
            // A bare value with a step (a/n) runs from a up to the field max.
            start = *value;
            end = step_part.has_value() ? maximum : *value;
        }

        if (start < minimum || end > maximum || start > end) return std::nullopt;
        for (int value = start; value <= end; value += step) result.insert(value);
    }
    if (result.empty()) return std::nullopt;
    return result;
}

}  // namespace

std::optional<CronExpression> CronExpression::parse(std::string_view expr) {
    const std::vector<std::string> parts = split_whitespace_runs(trim(expr));
    if (parts.size() != 5) return std::nullopt;

    const std::optional<std::set<int>> minutes = parse_field(parts[0], 0, 59);
    if (!minutes.has_value()) return std::nullopt;
    const std::optional<std::set<int>> hours = parse_field(parts[1], 0, 23);
    if (!hours.has_value()) return std::nullopt;
    const std::optional<std::set<int>> days_of_month = parse_field(parts[2], 1, 31);
    if (!days_of_month.has_value()) return std::nullopt;
    const std::optional<std::set<int>> months = parse_field(parts[3], 1, 12);
    if (!months.has_value()) return std::nullopt;

    const std::optional<std::set<int>> raw_days_of_week = parse_field(parts[4], 0, 7);
    if (!raw_days_of_week.has_value()) return std::nullopt;
    // toSet() over a mapped list keeps insertion order, so 7 folding to 0 lands where it lands.
    std::vector<int> days_of_week;
    for (int value : *raw_days_of_week) {
        const int mapped = value == 7 ? 0 : value;
        if (std::find(days_of_week.begin(), days_of_week.end(), mapped) == days_of_week.end()) {
            days_of_week.push_back(mapped);
        }
    }

    return CronExpression(*minutes, *hours, *days_of_month, *months, days_of_week,
                          /*dom_restricted=*/trim(parts[2]) != "*",
                          /*dow_restricted=*/trim(parts[4]) != "*");
}

bool CronExpression::is_valid(std::string_view expr) {
    return parse(expr).has_value();
}

bool CronExpression::day_matches(const tz::CivilTime& time) const {
    if (months_.find(time.month) == months_.end()) return false;
    const bool dom = days_of_month_.find(time.day) != days_of_month_.end();
    const bool dow =
        std::find(days_of_week_.begin(), days_of_week_.end(), time.weekday) != days_of_week_.end();
    return dom_restricted_ && dow_restricted_ ? (dom || dow) : (dom && dow);
}

std::optional<std::int64_t> CronExpression::next(std::int64_t after_millis,
                                                 const tz::Zone& zone) const {
    // Calendar truncates the seconds and milliseconds first, which moves BACKWARDS when they were
    // non-zero, and only then advances by one minute.
    std::int64_t instant = after_millis;
    const tz::CivilTime start = zone.civil_at(instant);
    const std::int64_t truncated = tz::wall_millis(
        tz::CivilTime{start.year, start.month, start.day, start.hour, start.minute, 0, 0});
    instant = zone.instant_from_wall(truncated);
    instant = zone.instant_from_wall(tz::wall_millis(zone.civil_at(instant)) + 60'000LL);

    const std::int64_t deadline = instant + kHorizonMillis;
    while (instant <= deadline) {
        const tz::CivilTime time = zone.civil_at(instant);
        if (!day_matches(time)) {
            instant = zone.instant_from_wall(
                tz::wall_millis(time) + 86'400'000LL);  // +1 day, then midnight
            const tz::CivilTime midnight = zone.civil_at(instant);
            instant = zone.instant_from_wall(
                tz::wall_millis(tz::CivilTime{midnight.year, midnight.month, midnight.day, 0, 0, 0, 0}));
            continue;
        }
        if (hours_.find(time.hour) == hours_.end()) {
            instant = zone.instant_from_wall(
                tz::wall_millis(time) - time.minute * 60'000LL + 3'600'000LL);  // +1 hour
            continue;
        }
        if (minutes_.find(time.minute) != minutes_.end()) return instant;
        instant = zone.instant_from_wall(tz::wall_millis(time) + 60'000LL);
    }
    return std::nullopt;
}

}  // namespace agora::core::cron