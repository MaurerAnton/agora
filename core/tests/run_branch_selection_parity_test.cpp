// Golden parity test for the native run-branch-selection policy.
//
// Replays every scenario under core/tools/golden/run-branch-selection/scenarios through
// agora::core::run_branch_selection and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/run-branch-selection/RunBranchSelectionTraceDump.kt).
//
// The pins that matter: a null parent key is not the empty string, a self-edge is dropped even when
// the entity claims to be its own parent, an empty input stays empty, selection order survives, the
// selection map keeps the last value for a repeated key, and `associateBy` resolves duplicate run
// ids to the **last** entity.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/run_branch_selection.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace branches = agora::core::run_branch_selection;
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

/** `n` for a null id, `s:` + escaped id for a real one: the two can never collide. */
std::string encode_key(const std::optional<std::string>& value) {
    return value.has_value() ? "s:" + esc(*value) : "n";
}

std::string encode_value(const std::string& value) { return "s:" + esc(value); }

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

std::vector<branches::Selection> selection_array(const json::Value& root) {
    const json::Value* value = field(root, "selections");
    if (is_null(value) || value->kind != Kind::Array) {
        scenario_fail("selections must be an array of pairs");
    }
    std::vector<branches::Selection> out;
    out.reserve(value->elements.size());
    for (const json::Value& element : value->elements) {
        if (element.kind != Kind::Array || element.elements.size() != 2) {
            scenario_fail("a selection must be a [parent, child] pair");
        }
        const json::Value& parent = element.elements[0];
        const json::Value& child = element.elements[1];
        std::optional<std::string> parent_id;
        if (!is_null(&parent)) {
            if (parent.kind != Kind::String) scenario_fail("a selection parent must be a string");
            parent_id = parent.text;
        }
        if (child.kind != Kind::String) scenario_fail("a selection child must be a string");
        out.push_back({parent_id, child.text});
    }
    return out;
}

std::vector<branches::RunRef> run_array(const json::Value& root) {
    const json::Value* value = field(root, "runs");
    if (is_null(value) || value->kind != Kind::Array) scenario_fail("runs must be an array");
    std::vector<branches::RunRef> out;
    out.reserve(value->elements.size());
    for (const json::Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail("a run must be an object");
        const std::optional<std::string> id = optional_text(element, "id");
        if (!id.has_value()) scenario_fail("a run needs an id");
        out.push_back({*id, optional_text(element, "parentRunId")});
    }
    return out;
}

/** LinkedHashMap.put: a repeated key keeps its first position and takes the last value. */
std::vector<branches::Selection> as_map(std::vector<branches::Selection> selections) {
    std::vector<branches::Selection> out;
    out.reserve(selections.size());
    for (branches::Selection& selection : selections) {
        auto found = std::find_if(out.begin(), out.end(), [&](const branches::Selection& kept) {
            return kept.parent_run_id == selection.parent_run_id;
        });
        if (found == out.end()) {
            out.push_back(selection);
        } else {
            found->child_run_id = selection.child_run_id;
        }
    }
    return out;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('|');
        out += parts[i];
    }
    return out;
}

std::string selection_line(const std::vector<branches::Selection>& selections) {
    std::vector<std::string> parts;
    parts.reserve(selections.size());
    for (const branches::Selection& selection : selections) {
        parts.push_back(encode_key(selection.parent_run_id) + "=>" +
                        encode_value(selection.child_run_id));
    }
    return join(parts);
}

std::string run_line(const std::vector<branches::RunRef>& runs) {
    std::vector<std::string> parts;
    parts.reserve(runs.size());
    for (const branches::RunRef& run : runs) {
        parts.push_back(encode_key(run.id) + "=>" + encode_key(run.parent_run_id));
    }
    return join(parts);
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = required_text(root, "op");
    if (op != "retainValidEdges") scenario_fail(stem + ": unknown op " + op);

    const std::vector<branches::Selection> input = as_map(selection_array(root));
    const std::vector<branches::RunRef> runs = run_array(root);
    const std::vector<branches::Selection> kept = branches::retain_valid_edges(input, runs);

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";
    trace += "selections|" + selection_line(input) + "\n";
    trace += "runs|" + run_line(runs) + "\n";
    trace += "out|map|" + selection_line(kept) + "\n";
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

TEST(run_branch_selection_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_RUN_BRANCH_SELECTION_DIR;
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
