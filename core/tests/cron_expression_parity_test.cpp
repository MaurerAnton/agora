// Golden parity test for the native CronExpression port.
//
// Replays every scenario under core/tools/golden/cron-expression/scenarios through
// agora::core::cron::CronExpression and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/cron-expression/CronTraceDump.kt).
//
// Two things are worth calling out. Validity alone would hide nearly the whole grammar, so each
// parse trace also carries the five expanded value sets and the two restriction flags. And the
// resolution traces are where java.util.Calendar decides the answer: seconds are truncated
// backwards before the search advances, a wall time inside a spring-forward gap is never reached,
// and an ambiguous wall time resolves to standard time.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/cron_expression.hpp"
#include "agora/core/json.hpp"
#include "agora/core/tz.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace cron = agora::core::cron;
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

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::string required_field(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) scenario_fail(std::string(name) + ": missing");
    return value->text;
}

template <class Values>
std::string join(const Values& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ',';
        out += std::to_string(value);
    }
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = field(root, "name") != nullptr && field(root, "name")->kind == Kind::String
                                 ? field(root, "name")->text
                                 : stem;
    const std::string expr = required_field(root, "expr");
    const std::optional<cron::CronExpression> parsed_expr = cron::CronExpression::parse(expr);

    std::string trace = "name|" + esc(name) + "\n";
    trace += "expr|" + esc(expr) + "\n";
    trace += std::string("valid|") + (parsed_expr.has_value() ? '1' : '0') + "\n";
    if (!parsed_expr.has_value()) return trace;

    trace += "minutes|" + esc(join(parsed_expr->minutes())) + "\n";
    trace += "hours|" + esc(join(parsed_expr->hours())) + "\n";
    trace += "daysOfMonth|" + esc(join(parsed_expr->days_of_month())) + "\n";
    trace += "months|" + esc(join(parsed_expr->months())) + "\n";
    trace += "daysOfWeek|" + esc(join(parsed_expr->days_of_week())) + "\n";
    trace += std::string("domRestricted|") + (parsed_expr->dom_restricted() ? '1' : '0') + "\n";
    trace += std::string("dowRestricted|") + (parsed_expr->dow_restricted() ? '1' : '0') + "\n";

    const json::Value* wants_next = field(root, "next");
    if (wants_next == nullptr || wants_next->text != "true") return trace;

    const json::Value* after = field(root, "after_millis");
    if (after == nullptr) scenario_fail(stem + ": after_millis is required for op=next");
    const std::string requested_zone =
        field(root, "zone") != nullptr ? field(root, "zone")->text : std::string("UTC");

    const agora::core::tz::Zone& zone = agora::core::tz::zone_for(requested_zone);
    // TimeZone.getTimeZone silently yields GMT for an unknown id, and getID reports that.
    trace += "zone|" + esc(zone.known() ? zone.id() : std::string("GMT")) + "\n";
    const std::optional<std::int64_t> result = parsed_expr->next(std::stoll(after->text), zone);
    trace += "next|" + (result.has_value() ? std::to_string(*result) : std::string("-")) + "\n";
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

TEST(cron_expression_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CRON_EXPRESSION_DIR;
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