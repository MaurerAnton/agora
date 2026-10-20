// Golden parity test for the v16->v17 SQL migration.
//
// The scenario supplies the same conversation/message projections the Kotlin recording database
// answers. The C++ Database shim records every execSQL/query with typed bind arguments and the
// trace (calls JSON + exception) must equal the frozen Kotlin trace byte for byte.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/migration_16_17.hpp"
#include "check.hpp"

namespace {

namespace migration = agora::core::migration_16_17;
namespace json = agora::core::json;

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

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == json::Kind::Literal && value->text == "null");
}

std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::optional<std::int64_t> number(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind != json::Kind::Literal) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value->text, &consumed);
        if (consumed != value->text.size()) return std::nullopt;
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<const json::Value*> objects(const json::Value& root, std::string_view name) {
    std::vector<const json::Value*> found;
    const json::Value* value = field(root, name);
    if (value == nullptr || value->kind != json::Kind::Array) return found;
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::Object) scenario_fail(std::string(name) + " item not object");
        found.push_back(&element);
    }
    return found;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

struct ConversationRow {
    std::string id;
    std::optional<std::string> selected_branches_json;
};

struct MessageRow {
    std::string conversation_id;
    std::string id;
    std::optional<std::string> parent_id;
    std::string participant;
    std::string status;
    long long timestamp = 0;
};

std::string render_arg(const migration::SqlValue& value) {
    switch (value.kind) {
        case migration::SqlValue::Kind::Null: return "null";
        case migration::SqlValue::Kind::Text: return "s:" + value.text;
        case migration::SqlValue::Kind::Long: return "l:" + std::to_string(value.number);
        case migration::SqlValue::Kind::Int: return "i:" + std::to_string(value.number);
    }
    return "null";
}

migration::SqlValue sql_text(std::string value) {
    return migration::SqlValue::text_value(std::move(value));
}

migration::SqlValue sql_long(long long value) {
    return migration::SqlValue::long_value(value);
}

class RecordingDatabase : public migration::Database {
public:
    RecordingDatabase(std::vector<ConversationRow> conversations,
                      std::vector<MessageRow> messages)
        : conversations_(std::move(conversations)), messages_(std::move(messages)) {}

    std::vector<std::vector<std::string>> calls;

    void exec_sql(std::string_view sql, const std::vector<migration::SqlValue>& args) override {
        record("execSQL", sql, args);
        if (starts_with(sql, "UPDATE messages") && args.size() > 3) {
            assigned_.insert(args[3].text);
        }
    }

    migration::QueryResult query(std::string_view sql,
                                 const std::vector<migration::SqlValue>& args) override {
        record("query", sql, args);
        migration::QueryResult result;
        if (starts_with(sql, "SELECT id, selectedBranchesJson FROM conversations")) {
            result.columns = {"id", "selectedBranchesJson"};
            for (const ConversationRow& row : conversations_) {
                std::vector<migration::SqlValue> values;
                values.push_back(sql_text(row.id));
                values.push_back(row.selected_branches_json.has_value()
                                     ? sql_text(*row.selected_branches_json)
                                     : migration::SqlValue::null());
                result.rows.push_back(std::move(values));
            }
            return result;
        }
        if (starts_with(sql, "SELECT id, parentId, participant, status, timestamp")) {
            result.columns = {"id", "parentId", "participant", "status", "timestamp"};
            const std::string conversation_id = args.empty() ? "" : args[0].text;
            for (const MessageRow& row : messages_) {
                if (row.conversation_id != conversation_id) continue;
                std::vector<migration::SqlValue> values;
                values.push_back(sql_text(row.id));
                values.push_back(row.parent_id.has_value() ? sql_text(*row.parent_id)
                                                           : migration::SqlValue::null());
                values.push_back(sql_text(row.participant));
                values.push_back(sql_text(row.status));
                values.push_back(sql_long(row.timestamp));
                result.rows.push_back(std::move(values));
            }
            return result;
        }
        if (starts_with(sql, "SELECT COUNT(*) FROM messages WHERE runId IS NULL")) {
            result.columns = {"COUNT(*)"};
            long long missing = 0;
            for (const MessageRow& row : messages_) {
                if (assigned_.find(row.id) == assigned_.end()) ++missing;
            }
            result.rows.push_back({sql_long(missing)});
            return result;
        }
        throw std::runtime_error("unexpected query: " + std::string(sql));
    }

private:
    void record(std::string_view kind, std::string_view sql,
                const std::vector<migration::SqlValue>& args) {
        std::vector<std::string> call;
        call.emplace_back(kind);
        call.emplace_back(sql);
        for (const migration::SqlValue& arg : args) call.push_back(render_arg(arg));
        calls.push_back(std::move(call));
    }

    std::vector<ConversationRow> conversations_;
    std::vector<MessageRow> messages_;
    std::unordered_set<std::string> assigned_;
};

std::string render_calls(const std::vector<std::vector<std::string>>& calls) {
    json::Value array;
    array.kind = json::Kind::Array;
    for (const std::vector<std::string>& call : calls) {
        json::Value call_value;
        call_value.kind = json::Kind::Array;
        for (const std::string& part : call) {
            json::Value text;
            text.kind = json::Kind::String;
            text.text = part;
            call_value.elements.push_back(std::move(text));
        }
        array.elements.push_back(std::move(call_value));
    }
    return json::to_kotlin_string(array);
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem + ": not an object");
    }
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = optional_text(root, "op").value_or("");
    if (op != "migrate") scenario_fail(stem + ": unknown op " + op);

    std::vector<ConversationRow> conversations;
    for (const json::Value* item : objects(root, "conversations")) {
        ConversationRow row;
        row.id = required_text(*item, "id");
        row.selected_branches_json = optional_text(*item, "selectedBranchesJson");
        conversations.push_back(std::move(row));
    }
    std::vector<MessageRow> messages;
    for (const json::Value* item : objects(root, "messages")) {
        MessageRow row;
        row.conversation_id = required_text(*item, "conversationId");
        row.id = required_text(*item, "id");
        row.parent_id = optional_text(*item, "parentId");
        row.participant = required_text(*item, "participant");
        row.status = required_text(*item, "status");
        row.timestamp = number(*item, "timestamp").value_or(0);
        messages.push_back(std::move(row));
    }

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";
    trace += "args|" + std::to_string(conversations.size()) + "|" +
             std::to_string(messages.size()) + "\n";

    RecordingDatabase database(conversations, messages);
    std::string error = "-";
    try {
        migration::migrate_16_17(database);
    } catch (const migration::MigrationException& exception) {
        error = exception.class_name + ": " + exception.message;
    }
    trace += "result|" + esc(render_calls(database.calls)) + "|" + esc(error) + "\n";
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

TEST(migration_16_17_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MIGRATION_16_17_DIR;
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
