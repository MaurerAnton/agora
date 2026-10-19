// Golden parity test for the native TaskSchedule port.
//
// Replays every scenario under core/tools/golden/task-schedule/scenarios through
// agora::core::schedule and requires the produced trace to equal the frozen Kotlin trace byte for
// byte. The trace format mirrors the reference driver
// (core/tools/golden/task-schedule/TaskScheduleTraceDump.kt).
//
// Two deliberate design choices:
//
//   - The port takes the zone and the current instant explicitly (schedule::Context). The Kotlin
//     reads Calendar.getInstance() and System.currentTimeMillis() directly. The stand's scenarios
//     avoid those code paths -- see the README for exactly which -- so the test passes `now = 0`.
//     If a scenario ever did reach the clock, the port would disagree and the parity test would
//     catch it rather than hiding it.
//   - Month names come from a table in this test, not from the core. The Kotlin hands the pattern
//     to SimpleDateFormat with Locale.getDefault(), which is presentation data; the port asks for
//     the name and lets the caller decide which locale to answer with.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/task_schedule.hpp"
#include "agora/core/tz.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace schedule = agora::core::schedule;
using json::Kind;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** The stand's trace escaping, identical to esc() in the reference driver. */
std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

// SimpleDateFormat's abbreviated month names, which is what "MMM" renders.
const std::map<std::string, std::array<std::string, 12>>& month_tables() {
    static const std::map<std::string, std::array<std::string, 12>> tables = {
        {"en-US", {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"}},
        {"de-DE", {"Jan", "Feb", "März", "Apr", "Mai", "Jun",
                   "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"}},
        {"fr-FR", {"janv.", "févr.", "mars", "avr.", "mai", "juin",
                   "juil.", "août", "sept.", "oct.", "nov.", "déc."}},
    };
    return tables;
}

schedule::ScheduleType type_of(std::string_view name) {
    if (name == "ONCE") return schedule::ScheduleType::once;
    if (name == "WEEKLY") return schedule::ScheduleType::weekly;
    if (name == "MONTHLY") return schedule::ScheduleType::monthly;
    if (name == "YEARLY") return schedule::ScheduleType::yearly;
    return schedule::ScheduleType::daily;
}

const char* type_name(schedule::ScheduleType type) {
    switch (type) {
        case schedule::ScheduleType::once: return "ONCE";
        case schedule::ScheduleType::daily: return "DAILY";
        case schedule::ScheduleType::weekly: return "WEEKLY";
        case schedule::ScheduleType::monthly: return "MONTHLY";
        case schedule::ScheduleType::yearly: return "YEARLY";
    }
    return "DAILY";
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

std::string text_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return "";
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

long long int_field(const json::Value& object, std::string_view name, long long fallback) {
    const json::Value* value = field(object, name);
    return is_null(value) ? fallback : std::stoll(value->text);
}

std::optional<std::int64_t> long_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    return std::stoll(value->text);
}

std::string show(const schedule::TaskSchedule& value) {
    std::string days;
    for (int day : value.days_of_week) {
        if (!days.empty()) days += ',';
        days += std::to_string(day);
    }
    return std::string(type_name(value.type)) + "," + std::to_string(value.hour) + "," +
           std::to_string(value.minute) + "," + days + "," + std::to_string(value.day_of_month) +
           "," + std::to_string(value.month) + "," + std::to_string(value.once_at_millis);
}

std::string opt(std::optional<std::int64_t> value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = field(root, "name") != nullptr ? text_field(root, "name") : stem;
    const std::string op = text_field(root, "op");
    const std::string zone_id = field(root, "tz") != nullptr ? text_field(root, "tz") : "UTC";
    const std::string locale = field(root, "locale") != nullptr ? text_field(root, "locale") : "en-US";

    const json::Value* node = field(root, "schedule");
    schedule::TaskSchedule input;
    if (node != nullptr && !is_null(node)) {
        input.type = type_of(text_field(*node, "type"));
        input.hour = static_cast<int>(int_field(*node, "hour", 0));
        input.minute = static_cast<int>(int_field(*node, "minute", 0));
        input.day_of_month = static_cast<int>(int_field(*node, "dayOfMonth", 1));
        input.month = static_cast<int>(int_field(*node, "month", 1));
        input.once_at_millis = int_field(*node, "onceAtMillis", 0);
        if (const json::Value* days = field(*node, "daysOfWeek"); !is_null(days)) {
            for (const json::Value& day : days->elements) {
                input.days_of_week.push_back(std::stoi(day.text));
            }
        }
    }

    const agora::core::tz::Zone& zone = agora::core::tz::zone_for(zone_id);
    // The pinned scenarios never read the clock; see the header comment.
    const schedule::Context context(zone, 0);

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";
    trace += "tz|" + esc(zone.known() ? zone.id() : std::string("GMT")) + "\n";
    trace += "locale|" + esc(locale) + "\n";
    trace += "schedule|" + (node != nullptr && !is_null(node) ? show(input) : std::string("-")) + "\n";

    const std::string cron_expr = field(root, "cronExpr") != nullptr ? text_field(root, "cronExpr") : "";
    const std::optional<std::int64_t> run_at = long_field(root, "runAt");

    const auto out_schedule = [&](const schedule::TaskSchedule& value) {
        trace += "out|schedule|" + show(value) + "\n";
    };

    if (op == "toCron") {
        trace += "out|string|" + esc(schedule::to_cron(input)) + "\n";
    } else if (op == "toRunAt") {
        trace += "out|long|" + opt(schedule::to_run_at(input)) + "\n";
    } else if (op == "parse") {
        trace += "args|" + esc(cron_expr) + "|" + opt(run_at) + "\n";
        const std::optional<schedule::TaskSchedule> parsed_schedule =
            schedule::parse(cron_expr, run_at, context);
        trace += "out|schedule|" + (parsed_schedule.has_value() ? show(*parsed_schedule) : "-") + "\n";
    } else if (op == "switchedTo") {
        trace += "args|" + std::string(type_name(type_of(text_field(root, "newType")))) + "\n";
        out_schedule(schedule::switched_to(input, type_of(text_field(root, "newType")), context));
    } else if (op == "withOnceAt") {
        const int year = static_cast<int>(int_field(root, "year", 0));
        const int month = static_cast<int>(int_field(root, "month", 1));
        const int day = static_cast<int>(int_field(root, "day", 1));
        trace += "args|" + std::to_string(year) + "|" + std::to_string(month) + "|" +
                 std::to_string(day) + "\n";
        out_schedule(schedule::with_once_at(input, year, month, day, context));
    } else if (op == "withTime") {
        const int hour = static_cast<int>(int_field(root, "newHour", 0));
        const int minute = static_cast<int>(int_field(root, "newMinute", 0));
        trace += "args|" + std::to_string(hour) + "|" + std::to_string(minute) + "\n";
        out_schedule(schedule::with_time(input, hour, minute, context));
    } else if (op == "formatOnDate") {
        const auto table = month_tables().find(locale);
        if (table == month_tables().end()) scenario_fail("no month table for locale " + locale);
        const std::array<std::string, 12>& names = table->second;
        trace += "out|string|" +
                 esc(schedule::format_on_date(input, context,
                                             [&names](int month) { return names[month - 1]; })) +
                 "\n";
    } else if (op == "hasSchedule") {
        trace += "args|" + esc(cron_expr) + "|" + opt(run_at) + "\n";
        trace += std::string("out|bool|") + (schedule::has_schedule(cron_expr, run_at) ? "1" : "0") + "\n";
    } else {
        scenario_fail(stem + ": unknown op " + op);
    }
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(200, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(task_schedule_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_TASK_SCHEDULE_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}