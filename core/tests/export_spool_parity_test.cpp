// Golden parity test for ExportSpool (spool writer/index and streaming reader).
//
// Each scenario replays the real Kotlin trace: spool bytes, the layout index (with Kotlin's flush
// points driving the offset counter) and the reader probes — clamped slices, malformed records,
// lines crossing chunk/slice boundaries, and the iterator's exhausted state machine.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/export_spool.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace spool = agora::core::export_spool;
namespace json = agora::core::json;

using json::Kind;
using json::Member;
using json::Value;

constexpr const char* kSpoolFile = "platform_spool.tmp";

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

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::int64_t required_long(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Literal) {
        scenario_fail(std::string(name) + " must be a number");
    }
    try {
        return std::stoll(value->text);
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " is not a number");
    }
}

bool optional_flag(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a bool");
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return fallback;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string error_json(const spool::SpoolException& error) {
    std::string out = "{\"type\":";
    append_quoted(out, error.kotlin_type());
    out += ",\"message\":";
    append_quoted(out, error.what());
    out.push_back('}');
    return out;
}

// ------------------------------------------------------------------------ symbolic slices

spool::Slice resolve_slice(const Value& element, const spool::Index* index) {
    if (element.kind == Kind::String) {
        if (element.text.rfind("conv:", 0) == 0) {
            const auto position = static_cast<std::size_t>(std::stoul(element.text.substr(5)));
            if (index == nullptr || position >= index->conversations.size()) {
                scenario_fail("symbolic conv slice without a finished index");
            }
            return index->conversations[position].slice;
        }
        if (element.text.rfind("task:", 0) == 0) {
            const auto position = static_cast<std::size_t>(std::stoul(element.text.substr(5)));
            if (index == nullptr || position >= index->task_slices.size()) {
                scenario_fail("symbolic task slice without a finished index");
            }
            return index->task_slices[position];
        }
    }
    if (element.kind != Kind::Array || element.elements.size() != 2) {
        scenario_fail("bad slice");
    }
    return spool::Slice{std::stoll(element.elements[0].text),
                        std::stoll(element.elements[1].text)};
}

std::vector<spool::Slice> resolve_slices(const Value& element, const spool::Index* index) {
    if (element.kind != Kind::Array) scenario_fail("slices must be an array");
    std::vector<spool::Slice> slices;
    for (const Value& entry : element.elements) slices.push_back(resolve_slice(entry, index));
    return slices;
}

// ----------------------------------------------------------------------------- read probes

std::string run_read(const spool::SpoolReader& reader, const Value& spec,
                     const spool::Index* index) {
    const std::string kind = required_string(spec, "kind");
    const bool probe_next = optional_flag(spec, "probeNext", false);

    if (kind == "records") {
        const Value* slice = field(spec, "slice");
        if (slice == nullptr) scenario_fail("records read needs a slice");
        std::string entries;
        bool first_entry = true;
        std::optional<std::string> error;
        try {
            reader.read_records(resolve_slice(*slice, index),
                                [&](const spool::SpoolReader::Record& record) {
                                    if (!first_entry) entries.push_back(',');
                                    first_entry = false;
                                    entries += "{\"type\":";
                                    append_quoted(entries, record.type);
                                    entries += ",\"json\":";
                                    append_quoted(entries, record.json);
                                    entries.push_back('}');
                                });
        } catch (const spool::SpoolException& caught) {
            error = error_json(caught);
        }
        if (first_entry && !error.has_value()) return "{}";
        std::string out = "{";
        if (!first_entry) out += "\"records\":[" + entries + "]";
        if (error.has_value()) {
            if (!first_entry) out.push_back(',');
            out += "\"error\":" + *error;
        }
        out.push_back('}');
        return out;
    }

    if (kind != "iterator") scenario_fail("unknown read kind " + kind);
    const Value* slices_value = field(spec, "slices");
    if (slices_value == nullptr) scenario_fail("iterator read needs slices");
    std::optional<std::string> type_filter;
    if (const Value* type = field(spec, "type"); type != nullptr && type->kind == Kind::String) {
        type_filter = type->text;
    }

    std::vector<std::string> payloads;
    std::optional<std::string> error;
    std::optional<bool> has_next_after;
    std::optional<std::string> error_after;

    spool::SpoolReader::RecordIterator iterator =
        reader.record_iterator(resolve_slices(*slices_value, index), type_filter);
    try {
        while (iterator.has_next()) payloads.push_back(iterator.next());
    } catch (const spool::SpoolException& caught) {
        error = error_json(caught);
    }
    if (error.has_value()) {
        try {
            has_next_after = iterator.has_next();
        } catch (const spool::SpoolException& caught) {
            error_after = error_json(caught);
        }
    }
    if (probe_next) {
        try {
            payloads.push_back(iterator.next());
        } catch (const spool::SpoolException& caught) {
            error_after = error_json(caught);
        }
    }

    const auto list = [&](const std::vector<std::string>& values) {
        std::string out = "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out.push_back(',');
            append_quoted(out, values[i]);
        }
        out.push_back(']');
        return out;
    };
    std::string out = "{";
    bool first_key = true;
    if (!payloads.empty()) {
        out += "\"payloads\":" + list(payloads);
        first_key = false;
    }
    if (error.has_value()) {
        if (!first_key) out.push_back(',');
        out += "\"error\":" + *error;
        first_key = false;
    }
    if (has_next_after.has_value()) {
        if (!first_key) out.push_back(',');
        out += std::string("\"hasNextAfter\":") + (*has_next_after ? "true" : "false");
        first_key = false;
    }
    if (error_after.has_value()) {
        if (!first_key) out.push_back(',');
        out += "\"errorAfter\":" + *error_after;
    }
    out.push_back('}');
    return out;
}

// ------------------------------------------------------------------------------ the scenario

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;

    std::filesystem::remove(kSpoolFile);

    const bool has_writes = field(root, "writes") != nullptr;
    std::optional<spool::Index> index;
    std::optional<std::string> write_error;
    if (has_writes) {
        spool::SpoolWriter writer(kSpoolFile);
        try {
            const Value* writes = field(root, "writes");
            if (writes->kind != Kind::Array) scenario_fail("writes must be an array");
            for (const Value& element : writes->elements) {
                const std::string op = required_string(element, "op");
                const std::string payload = required_string(element, "json");
                if (op == "conversation") {
                    writer.write_conversation(required_string(element, "id"),
                                              required_long(element, "dataChangedAt"), payload);
                } else if (op == "run") {
                    writer.write_run(payload);
                } else if (op == "message") {
                    writer.write_message(payload);
                } else if (op == "loop") {
                    writer.write_loop(payload);
                } else if (op == "task") {
                    writer.write_task(payload);
                } else {
                    scenario_fail("unknown write op " + op);
                }
            }
            index = writer.finish();
        } catch (const spool::SpoolException& caught) {
            write_error = error_json(caught);
        }
        writer.close();
    } else {
        const std::string content =
            field(root, "content") != nullptr ? required_string(root, "content") : std::string();
        std::ofstream file(kSpoolFile, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.close();
    }

    const std::string bytes = read_file(kSpoolFile);

    std::string trace = "{\"bytes\":";
    append_quoted(trace, bytes);
    if (has_writes) {
        if (write_error.has_value()) {
            trace += ",\"writeError\":" + *write_error;
        } else {
            trace += ",\"index\":{\"conversations\":[";
            const spool::Index& finished = *index;
            for (std::size_t i = 0; i < finished.conversations.size(); ++i) {
                if (i != 0) trace.push_back(',');
                trace += "{\"id\":";
                append_quoted(trace, finished.conversations[i].id);
                trace += ",\"dataChangedAt\":" +
                         std::to_string(finished.conversations[i].data_changed_at);
                trace += ",\"start\":" + std::to_string(finished.conversations[i].slice.start_offset);
                trace += ",\"end\":" + std::to_string(finished.conversations[i].slice.end_offset);
                trace.push_back('}');
            }
            trace += "],\"taskSlices\":[";
            for (std::size_t i = 0; i < finished.task_slices.size(); ++i) {
                if (i != 0) trace.push_back(',');
                trace += "{\"start\":" + std::to_string(finished.task_slices[i].start_offset) +
                         ",\"end\":" + std::to_string(finished.task_slices[i].end_offset) + "}";
            }
            trace += "],\"watermarks\":{";
            const auto watermarks = finished.watermarks();
            for (std::size_t i = 0; i < watermarks.size(); ++i) {
                if (i != 0) trace.push_back(',');
                append_quoted(trace, watermarks[i].first);
                trace += ":" + std::to_string(watermarks[i].second);
            }
            trace += "}}";
        }
    }

    trace += ",\"reads\":[";
    if (!write_error.has_value()) {
        // After an aborted writer the Kotlin export stops; reads are skipped on both sides.
        const Value* reads = field(root, "reads");
        if (reads != nullptr) {
            if (reads->kind != Kind::Array) scenario_fail("reads must be an array");
            const spool::SpoolReader reader(kSpoolFile);
            const spool::Index* index_ptr = index.has_value() ? &*index : nullptr;
            for (std::size_t i = 0; i < reads->elements.size(); ++i) {
                if (i != 0) trace.push_back(',');
                trace += run_read(reader, reads->elements[i], index_ptr);
            }
        }
    }
    trace += "]}";
    std::filesystem::remove(kSpoolFile);
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

TEST(export_spool_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_EXPORT_SPOOL_DIR;
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
