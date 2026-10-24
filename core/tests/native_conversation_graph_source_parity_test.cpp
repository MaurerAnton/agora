// Golden parity tests for data/NativeConversationGraphSource.kt.
//
// The Kotlin stand (core/tools/golden/native-conversation-graph-source) compiles the source file
// verbatim over vendored AOSP android.util.Json* classes and freezes one trace per scenario:
// the chained graph content (Android's writer emission), every failure path with its JVM class
// name and message (require/error/checkNotNull, kotlinx index decode, AOSP MalformedJsonException
// line/column, EOFException, drain faults), the read-after-close contract of the handed-out
// stream, and the spool cleanup in the cache directory. This test replays each scenario through
// the C++ port and requires the trace to match at the JSON value level.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <map>
#include <unistd.h>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/android_json.hpp"
#include "agora/core/json.hpp"
#include "agora/core/native_conversation_graph_source.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace native_graph = agora::core::native_graph;
using native_graph::EntrySource;
using native_graph::Exception;
using native_graph::NativeConversationGraphSource;
using json::Kind;
using json::Member;
using json::Value;

// ---- scenario/trace helpers --------------------------------------------------------------------

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
        if (entry.key == name) found = &entry.value;  // last duplicate wins
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

int required_int(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Literal) {
        scenario_fail(std::string(name) + " must be an int");
    }
    return std::stoi(value->text);
}

std::vector<std::string> string_array(const Value* value) {
    if (value == nullptr) return {};
    if (value->kind != Kind::Array) scenario_fail("expected a string array");
    std::vector<std::string> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) scenario_fail("expected a string element");
        out.push_back(element.text);
    }
    return out;
}

json::Value json_string(std::string_view text) {
    Value v;
    v.kind = Kind::String;
    v.text = std::string(text);
    return v;
}

json::Value json_long(std::int64_t number) {
    Value v;
    v.kind = Kind::Literal;
    v.text = std::to_string(number);
    return v;
}

json::Value json_array(std::vector<json::Value> elements) {
    Value v;
    v.kind = Kind::Array;
    v.elements = std::move(elements);
    return v;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    Value v;
    v.kind = Kind::Object;
    for (auto& [key, value] : members) v.members.push_back({std::move(key), std::move(value)});
    return v;
}

bool json_equal(const json::Value& left, const json::Value& right) {
    if (left.kind != right.kind) return false;
    if (left.kind == Kind::String || left.kind == Kind::Literal) {
        return left.text == right.text;
    }
    if (left.kind == Kind::Array) {
        if (left.elements.size() != right.elements.size()) return false;
        for (std::size_t i = 0; i < left.elements.size(); ++i) {
            if (!json_equal(left.elements[i], right.elements[i])) return false;
        }
        return true;
    }
    if (left.members.size() != right.members.size()) return false;
    for (const json::Member& member : left.members) {
        const json::Value* match = field(right, member.key);
        if (match == nullptr || !json_equal(member.value, *match)) return false;
    }
    return true;
}

std::string excerpt(const json::Value& value, std::size_t limit = 400) {
    std::string text = json::to_kotlin_string(value);
    if (text.size() > limit) return text.substr(0, limit) + "...";
    return text;
}

// ---- the entry-source double -------------------------------------------------------------------

/**
 * The stand's NativeGraphEntrySource double: scenario bytes in memory, streams with the JDK
 * contracts the traces pin — read after close throws IOException "Stream Closed" (a
 * FileInputStream), EOF reports -1, and a drain fault throws at the end instead of returning -1.
 */
class MemoryStream final : public agora::core::android_json::Source {
public:
    MemoryStream(std::string bytes, std::optional<std::string> drain_fault)
        : bytes_(std::move(bytes)), drain_fault_(std::move(drain_fault)) {}

    long long read(char* buffer, long long capacity) override {
        if (capacity <= 0) return 0;
        if (closed_) throw Exception("IOException", "Stream Closed");
        if (pos_ < bytes_.size()) {
            const std::size_t take =
                std::min<std::size_t>(bytes_.size() - pos_, static_cast<std::size_t>(capacity));
            std::memcpy(buffer, bytes_.data() + pos_, take);
            pos_ += take;
            return static_cast<long long>(take);
        }
        if (drain_fault_.has_value()) throw Exception("IOException", *drain_fault_);
        return -1;
    }

    void close() override { closed_ = true; }

private:
    std::string bytes_;
    std::optional<std::string> drain_fault_;
    std::size_t pos_ = 0;
    bool closed_ = false;
};

class MapEntrySource final : public EntrySource {
public:
    std::map<std::string, std::string> entries;
    std::set<std::string> stream_nulls;
    std::map<std::string, std::string> drain_faults;

    bool has(std::string_view name) override {
        return entries.find(std::string(name)) != entries.end();
    }

    std::optional<std::string> bytes(std::string_view name) override {
        const auto it = entries.find(std::string(name));
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }

    std::unique_ptr<agora::core::android_json::Source> stream(std::string_view name) override {
        const std::string key(name);
        const auto it = entries.find(key);
        if (it == entries.end() || stream_nulls.count(key) != 0) return nullptr;
        const auto fault = drain_faults.find(key);
        return std::make_unique<MemoryStream>(
            it->second, fault == drain_faults.end() ? std::nullopt
                                                    : std::optional<std::string>(fault->second));
    }
};

// ---- scenario runner ---------------------------------------------------------------------------

std::string read_after_close(NativeConversationGraphSource::Stream& stream) {
    char byte = 0;
    try {
        const long long got = stream.read(&byte, 1);
        if (got < 0) return "eof";
        return "data:" + std::to_string(static_cast<unsigned char>(byte));
    } catch (const Exception& error) {
        return error.java_class() + ": " + std::string(error.what());
    }
}

json::Value json_error(const std::string& type, const std::string& message) {
    return json_object({{"type", json_string(type)}, {"message", json_string(message)}});
}

void run_scenario(const std::filesystem::path& scenario_path,
                  const std::filesystem::path& golden_path) {
    const json::ParseResult scenario = json::parse(read_file(scenario_path));
    if (!scenario.value || scenario.value->kind != Kind::Object) {
        scenario_fail("scenario is not an object: " + scenario_path.filename().string());
    }
    const Value& root = *scenario.value;

    const int version = required_int(root, "version");
    MapEntrySource archive;
    if (const Value* entries = field(root, "entries");
        entries != nullptr && entries->kind == Kind::Object) {
        for (const Member& member : entries->members) {
            if (member.value.kind != Kind::String) scenario_fail("entries must be strings");
            archive.entries[member.key] = member.value.text;
        }
    }
    const std::vector<std::string> stream_nulls = string_array(field(root, "streamNull"));
    archive.stream_nulls = std::set<std::string>(stream_nulls.begin(), stream_nulls.end());
    if (const Value* faults = field(root, "drainFaults");
        faults != nullptr && faults->kind == Kind::Object) {
        for (const Member& member : faults->members) {
            archive.drain_faults[member.key] = member.value.text;
        }
    }

    static int scenario_counter = 0;
    const std::filesystem::path cache_dir = std::filesystem::temp_directory_path() /
        ("agora-graph-source-cpp-" + std::to_string(scenario_counter++) + "-" +
         std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(cache_dir, ignored);
    std::filesystem::create_directories(cache_dir);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{cache_dir};

    const Value* steps = field(root, "steps");
    if (steps == nullptr || steps->kind != Kind::Array) scenario_fail("scenario has no steps");

    std::vector<json::Value> trace_steps;
    for (const Value& step : steps->elements) {
        const std::string op = required_string(step, "op");

        json::Value result;
        NativeConversationGraphSource source;
        bool opened = false;
        try {
            source = NativeConversationGraphSource::open(archive, version, cache_dir);
            opened = true;
            NativeConversationGraphSource::Stream stream = source.open_stream();
            if (op == "open") {
                const std::string content = stream.read_all();
                source.close();
                result = json_object({
                    {"content", json_string(content)},
                    {"readAfterClose", json_string(read_after_close(stream))},
                });
            } else if (op == "open-partial") {
                char byte = 0;
                const long long got = stream.read(&byte, 1);
                source.close();
                result = json_object({
                    {"firstByte", json_long(got < 0 ? -1
                                                    : static_cast<unsigned char>(byte))},
                    {"readAfterClose", json_string(read_after_close(stream))},
                });
            } else {
                scenario_fail("unknown step op " + op);
            }
            opened = false;  // already closed inside the op
        } catch (const Exception& error) {
            result = json_object(
                {{"error", json_error(error.java_class(), error.what())}});
        } catch (const std::exception& error) {
            result = json_object({{"error", json_error("RuntimeException", error.what())}});
        }
        if (opened) source.close();

        std::vector<std::string> cache;
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir)) {
            cache.push_back(entry.path().filename().string());
        }
        std::sort(cache.begin(), cache.end());
        std::vector<json::Value> cache_values;
        for (const auto& name : cache) cache_values.push_back(json_string(name));

        trace_steps.push_back(json_object({
            {"op", json_string(op)},
            {"result", std::move(result)},
            {"cache", json_array(std::move(cache_values))},
        }));
    }

    const json::Value actual = json_object({{"steps", json_array(std::move(trace_steps))}});
    const json::ParseResult golden = json::parse(read_file(golden_path));
    if (!golden.value) scenario_fail("golden trace is not JSON: " + golden_path.string());
    if (!json_equal(actual, *golden.value)) {
        scenario_fail(scenario_path.filename().string() + "\n  expected: " +
                      excerpt(*golden.value) + "\n  actual:   " + excerpt(actual));
    }
}

void test_native_conversation_graph_source_golden_parity() {
    const std::filesystem::path root = AGORA_FIXTURE_NATIVE_CONVERSATION_GRAPH_SOURCE_DIR;
    const std::filesystem::path scenario_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenario_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            scenarios.push_back(entry.path());
        }
    }
    std::sort(scenarios.begin(), scenarios.end());
    if (scenarios.empty()) scenario_fail("no graph-source scenarios found");
    for (const std::filesystem::path& scenario : scenarios) {
        run_scenario(scenario, golden_dir / (scenario.stem().string() + ".trace"));
    }
}

}  // namespace

TEST(native_conversation_graph_source_golden_parity) {
    test_native_conversation_graph_source_golden_parity();
}
