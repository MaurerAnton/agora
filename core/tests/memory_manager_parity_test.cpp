// Golden parity test for the C++ MemoryManager port.
//
// Replays every scenario under core/tools/golden/memory-manager/scenarios through
// agora::core::memory::MemoryManager and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/memory-manager/MemoryManagerTraceDump.kt), including the `setup_content` /
// `setup_target_*` / `active_seed` preparation steps and the recorded error messages.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/memory_manager.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace memory = agora::core::memory;

using MemoryManager = memory::MemoryManager;

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
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) return &entry.value;
    }
    return nullptr;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == json::Kind::Literal && value->text == "null");
}

std::string string_or(const json::Value& object, std::string_view name,
                      const std::string& fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name), "expected a string");
    return value->text;
}

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name), "expected a string");
    return value->text;
}

/** True when the scenario provides a non-blank string, matching Kotlin's String.isNotBlank(). */
bool has_text(const json::Value& object, std::string_view name) {
    const std::string value = string_or(object, name, "");
    return std::any_of(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c) == 0; });
}

/** The driver maps blank strings to null via `takeIf { it.isNotBlank() }`. */
std::optional<std::string> optional_nonblank(const json::Value& object, std::string_view name) {
    if (!has_text(object, name)) return std::nullopt;
    return string_or(object, name, "");
}

std::string run_scenario(const std::filesystem::path& scenario_path,
                         const std::filesystem::path& temp_root) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string name = string_or(scenario, "name", stem);
    if (name != stem) {
        scenario_fail(stem, "scenario name '" + name + "' must match the file stem");
    }
    const std::string mode = string_or(scenario, "mode", "active");

    std::ostringstream trace;
    const auto line = [&](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& value : fields) {
            if (!first) trace << '|';
            first = false;
            trace << value;
        }
        trace << '\n';
    };

    line({"name", esc(name)});
    line({"mode", esc(mode)});

    // The driver builds one private working directory per scenario; wipe it so re-runs are
    // idempotent, exactly like run_golden.sh does for the Kotlin side.
    const std::filesystem::path files_dir = temp_root / ("scenario_" + name);
    std::filesystem::remove_all(files_dir);
    std::filesystem::create_directories(files_dir);

    MemoryManager manager(files_dir.string());

    try {
        // Preparation steps the driver performs before the operation under test.
        if (has_text(scenario, "setup_content")) {
            manager.create_file(string_or(scenario, "file_name", ""),
                                string_or(scenario, "setup_content", ""));
        }
        if (has_text(scenario, "setup_target_name")) {
            manager.create_file(string_or(scenario, "setup_target_name", ""),
                                string_or(scenario, "setup_target_content", ""));
        }
        if (has_text(scenario, "active_seed")) {
            manager.update_active_memory(string_or(scenario, "active_seed", ""));
        }

        if (mode == "active") {
            line({"result",
                  esc(manager.update_active_memory(
                      string_or(scenario, "content_param", ""),
                      string_or(scenario, "mode_param", "replace"),
                      optional_string(scenario, "old_string"),
                      optional_string(scenario, "new_string")))});
            line({"active", esc(manager.get_active_memory())});
        } else if (mode == "description_get") {
            line({"description", esc(manager.get_description(string_or(scenario, "file_name", "")))});
        } else if (mode == "description_set") {
            manager.set_description(string_or(scenario, "file_name", ""),
                                    string_or(scenario, "description", ""));
            line({"ok"});
        } else if (mode == "list") {
            const auto files = manager.list_files();
            line({"count", std::to_string(files.size())});
            for (const auto& file : files) line({"file", esc(file.name), esc(file.description)});
        } else if (mode == "meta") {
            line({"json", esc(manager.get_meta_json())});
        } else if (mode == "meta_save") {
            manager.save_meta_json(string_or(scenario, "meta_json", ""));
            line({"json", esc(manager.get_meta_json())});
        } else if (mode == "read") {
            line({"content", esc(manager.read_file(string_or(scenario, "file_name", "")))});
        } else if (mode == "create") {
            line({"result",
                  esc(manager.create_file(string_or(scenario, "file_name", ""),
                                          string_or(scenario, "content_param", ""),
                                          string_or(scenario, "description", "")))});
        } else if (mode == "edit") {
            line({"result",
                  esc(manager.edit_file(string_or(scenario, "file_name", ""),
                                        optional_nonblank(scenario, "content_param"),
                                        optional_nonblank(scenario, "new_name"),
                                        optional_nonblank(scenario, "description"),
                                        optional_string(scenario, "old_string"),
                                        optional_string(scenario, "new_string")))});
        } else if (mode == "delete") {
            line({"result", esc(manager.delete_file(string_or(scenario, "file_name", "")))});
        } else if (mode == "revision") {
            line({"active", std::to_string(manager.active_memory_revision())});
            line({"catalog", std::to_string(manager.catalog_revision())});
        } else {
            scenario_fail(stem, "unknown mode: " + mode);
        }
    } catch (const std::exception& error) {
        line({"error", esc(error.what())});
    }
    return trace.str();
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text, std::size_t begin) {
        return text.substr(begin, std::min<std::size_t>(160, text.size() - begin));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual, from)
            << "\n       expected: " << window(expected, from);
    return message.str();
}

}  // namespace

TEST(memory_manager_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MEMORY_MANAGER_DIR;
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

    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "agora_memory_manager_parity";
    std::filesystem::remove_all(temp_root);
    std::filesystem::create_directories(temp_root);

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario, temp_root);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));

    std::filesystem::remove_all(temp_root);
}
