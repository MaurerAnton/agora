// Golden parity test for the native custom-model-reference helpers.
//
// Replays every scenario under core/tools/golden/custom-model-references/scenarios through
// agora::core::custom_model_references and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/custom-model-references/CustomModelReferencesTraceDump.kt).
//
// The pins that matter are container-order pins: `buildSet` keeps a replacement in the position of
// the id it replaces, `remove` + insert moves a self-rename to the end, an existing target keeps its
// position, and `isNotBlank()` treats a no-break space as blank while a zero-width space is content.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agora/core/custom_model_references.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace refs = agora::core::custom_model_references;
using json::Kind;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '=': out += "\\e"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string opt(const std::optional<std::string>& value) {
    return value.has_value() ? esc(*value) : "-";
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('|');
        out += parts[i];
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

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

/** nullopt for an absent or JSON-null field, otherwise the string -- empty is a real value. */
std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::vector<std::string> string_array(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array");
    }
    std::vector<std::string> out;
    out.reserve(value->elements.size());
    for (const json::Value& element : value->elements) {
        if (element.kind != Kind::String) {
            scenario_fail(std::string(name) + " must contain strings");
        }
        out.push_back(element.text);
    }
    return out;
}

refs::OrderedMap entry_array(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array of pairs");
    }
    refs::OrderedMap out;
    out.reserve(value->elements.size());
    for (const json::Value& element : value->elements) {
        if (element.kind != Kind::Array || element.elements.size() != 2 ||
            element.elements[0].kind != Kind::String ||
            element.elements[1].kind != Kind::String) {
            scenario_fail(std::string(name) + " must contain [key, value] string pairs");
        }
        out.emplace_back(element.elements[0].text, element.elements[1].text);
    }
    return out;
}

/** LinkedHashSet.addAll: duplicates keep their first position. */
refs::OrderedSet as_set(const std::vector<std::string>& items) {
    refs::OrderedSet out;
    std::unordered_set<std::string> seen;
    out.reserve(items.size());
    for (const std::string& item : items) {
        if (seen.insert(item).second) out.push_back(item);
    }
    return out;
}

/** LinkedHashMap.put: a repeated key keeps its first position and takes the last value. */
refs::OrderedMap as_map(const refs::OrderedMap& entries) {
    refs::OrderedMap out;
    std::unordered_map<std::string, std::size_t> positions;
    out.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        const auto found = positions.find(key);
        if (found == positions.end()) {
            positions.emplace(key, out.size());
            out.emplace_back(key, value);
        } else {
            out[found->second].second = value;
        }
    }
    return out;
}

std::string set_line(const refs::OrderedSet& set) {
    std::vector<std::string> parts;
    parts.reserve(set.size());
    for (const std::string& item : set) parts.push_back(esc(item));
    return join(parts);
}

std::string map_line(const refs::OrderedMap& map) {
    std::vector<std::string> parts;
    parts.reserve(map.size());
    for (const auto& [key, value] : map) parts.push_back(esc(key) + "=>" + esc(value));
    return join(parts);
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = required_text(root, "op");
    const std::string old_model_id = required_text(root, "oldModelId");
    const std::optional<std::string> new_model_id = optional_text(root, "newModelId");

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";

    if (op == "set") {
        const std::vector<std::string> items = string_array(root, "items");
        const refs::OrderedSet input = as_set(items);
        const refs::OrderedSet result =
            refs::replace_model_reference(input, old_model_id, new_model_id);

        std::vector<std::string> raw_items;
        raw_items.reserve(items.size());
        for (const std::string& item : items) raw_items.push_back(esc(item));
        trace += "args|" + esc(old_model_id) + "|" + opt(new_model_id) + "|" + join(raw_items) + "\n";
        trace += "out|set|" + set_line(result) + "\n";
    } else if (op == "string") {
        const std::optional<std::string> value = optional_text(root, "value");
        const std::optional<std::string> result =
            refs::replace_model_reference(value, old_model_id, new_model_id);
        trace += "args|" + opt(value) + "|" + esc(old_model_id) + "|" + opt(new_model_id) + "\n";
        trace += "out|string?|" + opt(result) + "\n";
    } else if (op == "map") {
        const std::string alias = required_text(root, "alias");
        const refs::OrderedMap entries = entry_array(root, "entries");
        const refs::OrderedMap input = as_map(entries);
        const refs::OrderedMap result =
            refs::replace_custom_model_alias(input, old_model_id, new_model_id, alias);
        trace += "args|" + esc(old_model_id) + "|" + opt(new_model_id) + "|" + esc(alias) + "|" +
                 map_line(entries) + "\n";
        trace += "out|map|" + map_line(result) + "\n";
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

TEST(custom_model_references_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CUSTOM_MODEL_REFERENCES_DIR;
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
