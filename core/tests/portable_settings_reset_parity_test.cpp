// Golden parity test for the portable settings reset (the portable allowlist boundary).
//
// The scenario seeds a preference store with every key of the settings schema, runs the reset and
// compares the surviving keys, the removed keys and the rewritten container with the Kotlin trace.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/portable_settings_reset.hpp"
#include "check.hpp"

namespace {

namespace reset = agora::core::settings_reset;
namespace json = agora::core::json;

using json::Kind;
using json::Member;
using json::Value;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::vector<std::string> string_list(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    std::vector<std::string> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) scenario_fail(std::string(name) + " must be strings");
        out.push_back(element.text);
    }
    return out;
}

/** The in-memory preference store (the DataStore preferences contract). */
class RecordingStore : public reset::PreferenceStore {
public:
    std::vector<std::string> names;
    std::vector<std::pair<std::string, std::string>> written;

    void remove(std::string_view key) override {
        const std::string name(key);
        names.erase(std::remove(names.begin(), names.end(), name), names.end());
    }

    void set_raw(std::string_view key, std::string_view value) override {
        const std::string name(key);
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        for (auto& entry : written) {
            if (entry.first == name) {
                entry.second = std::string(value);
                return;
            }
        }
        written.emplace_back(name, std::string(value));
    }
};

std::string render_names(const std::vector<std::string>& names) {
    std::string out = "[";
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) out.push_back(',');
        out.push_back('"');
        json::escape_to(out, names[index]);
        out.push_back('"');
    }
    out.push_back(']');
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const std::vector<std::string> schema_keys = string_list(root, "schemaKeys");
    CHECK(!schema_keys.empty());

    RecordingStore store;
    store.names = schema_keys;
    reset::clear_portable_settings(store);

    const std::vector<std::string> removed = reset::removed_preference_names();
    std::vector<std::string> removed_in_order;
    for (const std::string& name : removed) {
        if (name == reset::kModelProviderNamesKey) continue;
        removed_in_order.push_back(name);
    }
    std::vector<std::string> missing;
    for (const std::string& name : removed) {
        if (std::find(schema_keys.begin(), schema_keys.end(), name) == schema_keys.end()) {
            missing.push_back(name);
        }
    }
    CHECK(missing.empty());

    std::vector<std::string> removed_in_schema_order;
    for (const std::string& name : schema_keys) {
        if (std::find(store.names.begin(), store.names.end(), name) == store.names.end()) {
            removed_in_schema_order.push_back(name);
        }
    }
    std::string trace = "{\"surviving\":" + render_names(store.names) + ",\"removed\":" +
                        render_names(removed_in_schema_order) + ",\"removedInOrder\":" +
                        render_names(removed_in_order) + ",\"written\":[";
    for (std::size_t index = 0; index < store.written.size(); ++index) {
        if (index != 0) trace.push_back(',');
        trace += "{\"key\":";
        trace.push_back('"');
        json::escape_to(trace, store.written[index].first);
        trace += "\",\"value\":";
        trace.push_back('"');
        json::escape_to(trace, store.written[index].second);
        trace += "\"}";
    }
    trace += "]}";
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(portable_settings_reset_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_PORTABLE_SETTINGS_RESET_DIR;
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
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
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
