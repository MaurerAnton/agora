#include "agora/core/tz.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

namespace agora::core::tz {
namespace {

// ---------------------------------------------------------------------------------------------
// Proleptic Gregorian civil arithmetic (Howard Hinnant's algorithms)
// ---------------------------------------------------------------------------------------------

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --quotient;
    return quotient;
}

std::int64_t days_from_civil(std::int64_t year, int month, int day) {
    year -= month <= 2;
    const std::int64_t era = floor_div(year, 400);
    const auto year_of_era = static_cast<std::int64_t>(year - era * 400);          // [0, 399]
    const std::int64_t day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;                 // [0, 365]
    const std::int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;                                    // days since epoch
}

void civil_from_days(std::int64_t days, std::int64_t& year, int& month, int& day) {
    days += 719468;
    const std::int64_t era = floor_div(days, 146097);
    const auto day_of_era = static_cast<std::int64_t>(days - era * 146097);       // [0, 146096]
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t year_value = year_of_era + era * 400;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);   // [0, 365]
    const std::int64_t mp = (5 * day_of_year + 2) / 153;                          // [0, 11]
    day = static_cast<int>(day_of_year - (153 * mp + 2) / 5 + 1);                // [1, 31]
    month = static_cast<int>(mp + (mp < 10 ? 3 : -9));                            // [1, 12]
    year = year_value + (month <= 2);
}

// ---------------------------------------------------------------------------------------------
// TZif reading
// ---------------------------------------------------------------------------------------------

std::int32_t read_be32(const unsigned char* at) {
    return (static_cast<std::int32_t>(at[0]) << 24) | (static_cast<std::int32_t>(at[1]) << 16) |
           (static_cast<std::int32_t>(at[2]) << 8) | static_cast<std::int32_t>(at[3]);
}

std::int64_t read_be64(const unsigned char* at) {
    std::int64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::int64_t>(at[index]);
    }
    return value;
}

/** Reads one TZif header plus its data block; `wide` selects 64-bit transition times. */
struct TzifBlock {
    std::int64_t isutcnt = 0;
    std::int64_t isstdcnt = 0;
    std::int64_t leapcnt = 0;
    std::int64_t timecnt = 0;
    std::int64_t typecnt = 0;
    std::int64_t charcnt = 0;
    std::size_t data_begin = 0;  // first byte after the six counts
};

bool read_header(const std::vector<unsigned char>& file, std::size_t at, TzifBlock& block) {
    if (at + 44 > file.size()) return false;
    if (std::memcmp(file.data() + at, "TZif", 4) != 0) return false;
    block.isutcnt = read_be32(file.data() + at + 20);
    block.isstdcnt = read_be32(file.data() + at + 24);
    block.leapcnt = read_be32(file.data() + at + 28);
    block.timecnt = read_be32(file.data() + at + 32);
    block.typecnt = read_be32(file.data() + at + 36);
    block.charcnt = read_be32(file.data() + at + 40);
    block.data_begin = at + 44;
    return true;
}

/** Byte length of the data block that follows a header. */
std::size_t block_size(const TzifBlock& block, bool wide) {
    const std::size_t time_size = wide ? 8 : 4;
    return static_cast<std::size_t>(block.timecnt) * time_size +
           static_cast<std::size_t>(block.timecnt) +
           static_cast<std::size_t>(block.typecnt) * 6 +
           static_cast<std::size_t>(block.charcnt) +
           static_cast<std::size_t>(block.leapcnt) * (wide ? 12 : 8) +
           static_cast<std::size_t>(block.isstdcnt) + static_cast<std::size_t>(block.isutcnt);
}

bool read_whole_file(const std::string& path, std::vector<unsigned char>& out) {
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) return false;
    out.clear();
    unsigned char buffer[8192];
    while (true) {
        const std::size_t got = std::fread(buffer, 1, sizeof buffer, handle);
        out.insert(out.end(), buffer, buffer + got);
        if (got < sizeof buffer) break;
    }
    std::fclose(handle);
    return true;
}

/**
 * The zone name is a path inside the tz database, so reject anything that could escape it.
 * JVM ids that are not file names -- "GMT+05:30", "UTC" -- are handled before this is called.
 */
bool zone_id_to_path(const std::string& id, std::string& path) {
    if (id.empty()) return false;
    if (id.find("..") != std::string::npos || id[0] == '/') return false;
    for (const char c : id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' ||
              c == '+')) {
            return false;
        }
    }
    path = "/usr/share/zoneinfo/" + id;
    return true;
}

}  // namespace

std::int64_t wall_millis(const CivilTime& time) {
    const std::int64_t days = days_from_civil(time.year, time.month, time.day);
    return days * 86400000 + time.hour * 3600000LL + time.minute * 60000LL +
           time.second * 1000LL + time.millis;
}

CivilTime civil_from_wall_millis(std::int64_t millis) {
    std::int64_t days = floor_div(millis, 86400000);
    std::int64_t rest = millis - days * 86400000;

    CivilTime time;
    civil_from_days(days, time.year, time.month, time.day);
    time.hour = static_cast<int>(rest / 3600000);
    rest -= static_cast<std::int64_t>(time.hour) * 3600000;
    time.minute = static_cast<int>(rest / 60000);
    rest -= static_cast<std::int64_t>(time.minute) * 60000;
    time.second = static_cast<int>(rest / 1000);
    time.millis = static_cast<int>(rest - static_cast<std::int64_t>(time.second) * 1000);
    // 1970-01-01 was a Thursday, which is 4 when Sunday is 0.
    time.weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);
    return time;
}

Zone::Zone(std::string id) : id_(std::move(id)) {
    std::string path;
    if (id_ == "UTC" || id_ == "GMT" || id_.empty()) {
        types_.push_back(Type{0, false});
        transitions_.push_back(Transition{std::numeric_limits<std::int64_t>::min() / 4, 0});
        return;  // fixed zero offset
    }
    if (id_.rfind("GMT", 0) == 0 || id_.rfind("UTC+", 0) == 0 || id_.rfind("UTC-", 0) == 0) {
        // "GMT+05:30" / "GMT-08:00": a JVM-only spelling of a fixed offset. Types 0-2 in the tz
        // database are exactly these, so map the sign onto them rather than reimplementing.
        const std::size_t colon = id_.find(':');
        int hours = 0;
        int minutes = 0;
        int sign = 0;
        if (colon != std::string::npos) {
            hours = std::stoi(id_.substr(3, colon - 3));
            minutes = std::stoi(id_.substr(colon + 1));
            sign = id_[3] == '-' ? -1 : 1;
        } else if (id_.size() > 3) {
            hours = std::stoi(id_.substr(3));
            sign = id_[3] == '-' ? -1 : 1;
        } else {
            return;
        }
        if (hours > 14 || minutes > 59) return;
        // Not the POSIX convention: TimeZone.getTimeZone("GMT+05:30") means 5h30m EAST, the same
        // reading as "Etc/GMT-5:30". Only the tz-file spelling inverts the sign.
        const std::int64_t offset = sign * (hours * 3600000LL + minutes * 60000LL);
        types_.push_back(Type{offset, false});
        transitions_.push_back(Transition{std::numeric_limits<std::int64_t>::min() / 4, 0});
        leading_type_ = 0;
        return;
    }
    if (!zone_id_to_path(id_, path)) return;

    std::vector<unsigned char> file;
    if (!read_whole_file(path, file)) return;

    TzifBlock narrow;
    if (!read_header(file, 0, narrow)) return;
    const bool wide = file[4] >= '2';
    TzifBlock block = narrow;
    std::size_t data = narrow.data_begin;
    if (wide) {
        // Skip the 32-bit block and re-read the 64-bit one.
        const std::size_t next = narrow.data_begin + block_size(narrow, false);
        TzifBlock wide_block;
        if (!read_header(file, next, wide_block)) return;
        block = wide_block;
        data = wide_block.data_begin;
    }

    const std::size_t time_width = wide ? 8 : 4;
    std::vector<std::int64_t> times;
    times.reserve(static_cast<std::size_t>(block.timecnt));
    for (std::int64_t index = 0; index < block.timecnt; ++index) {
        const unsigned char* at = file.data() + data + static_cast<std::size_t>(index) * time_width;
        times.push_back(wide ? read_be64(at) : read_be32(at));
    }
    data += static_cast<std::size_t>(block.timecnt) * time_width;

    std::vector<int> indices;
    indices.reserve(static_cast<std::size_t>(block.timecnt));
    for (std::int64_t index = 0; index < block.timecnt; ++index) {
        indices.push_back(file[data + static_cast<std::size_t>(index)]);
    }
    data += static_cast<std::size_t>(block.timecnt);

    types_.reserve(static_cast<std::size_t>(block.typecnt));
    for (std::int64_t index = 0; index < block.typecnt; ++index) {
        const unsigned char* at = file.data() + data + static_cast<std::size_t>(index) * 6;
        Type type;
        type.offset = read_be32(at) * 1000LL;
        type.is_dst = at[4] != 0;
        types_.push_back(type);
    }

    for (std::size_t index = 0; index < times.size(); ++index) {
        transitions_.push_back(Transition{times[index], indices[index]});
    }
    // Before the first transition the first non-DST type is in effect.
    leading_type_ = 0;
    for (std::size_t index = 0; index < types_.size(); ++index) {
        if (!types_[index].is_dst) {
            leading_type_ = static_cast<int>(index);
            break;
        }
    }
}

int Zone::active_type(std::int64_t utc_seconds) const {
    if (transitions_.empty()) return leading_type_;
    const auto found = std::upper_bound(
        transitions_.begin(), transitions_.end(), utc_seconds,
        [](std::int64_t value, const Transition& transition) { return value < transition.at; });
    if (found == transitions_.begin()) return leading_type_;
    return (found - 1)->type;
}

std::int64_t Zone::offset_at(std::int64_t utc_millis) const {
    if (types_.empty()) return 0;
    const std::int64_t seconds = floor_div(utc_millis, 1000);
    const int type = active_type(seconds);
    return types_[static_cast<std::size_t>(type)].offset;
}

std::int64_t Zone::standard_offset_for(std::int64_t wall) const {
    if (types_.empty()) return 0;
    // Guess the instant as if the wall time were UTC, then look for a type that is not daylight
    // time in the same part of the year: a zone's standard offset is the one that is in effect
    // when its daylight saving is off.
    const std::int64_t guess_seconds = floor_div(wall, 1000);
    const int active = active_type(guess_seconds);
    if (!types_[static_cast<std::size_t>(active)].is_dst) {
        return types_[static_cast<std::size_t>(active)].offset;
    }
    for (const std::int64_t shift : {182LL * 86400LL, -182LL * 86400LL, 91LL * 86400LL,
                                     -91LL * 86400LL}) {
        const int near = active_type(guess_seconds + shift);
        if (!types_[static_cast<std::size_t>(near)].is_dst) {
            return types_[static_cast<std::size_t>(near)].offset;
        }
    }
    return types_[static_cast<std::size_t>(active)].offset;
}

CivilTime Zone::civil_at(std::int64_t utc_millis) const {
    return civil_from_wall_millis(utc_millis + offset_at(utc_millis));
}

std::int64_t Zone::instant_from_wall(std::int64_t wall) const {
    if (types_.empty()) return wall;
    const std::int64_t standard = standard_offset_for(wall);
    const std::int64_t standard_guess = wall - standard;
    const std::int64_t own = offset_at(standard_guess);
    if (own == standard) return standard_guess;

    // The time is in daylight saving. Re-anchor on the offset that is actually in effect there
    // and accept it only if it really is -- that check is what distinguishes an ordinary summer
    // time from a wall clock reading inside a spring-forward gap or a fall-back overlap.
    const std::int64_t own_guess = wall - own;
    if (offset_at(own_guess) == own) return own_guess;
    return standard_guess;
}

namespace {

std::mutex& cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, Zone>& cache() {
    static std::map<std::string, Zone> zones;
    return zones;
}

}  // namespace

const Zone& zone_for(std::string_view id) {
    std::lock_guard<std::mutex> guard(cache_mutex());
    auto& zones = cache();
    const std::string key(id);
    const auto found = zones.find(key);
    if (found != zones.end()) return found->second;
    return zones.emplace(key, Zone(key)).first->second;
}

}  // namespace agora::core::tz