// tools/scenarios_main.cpp — C++ replacement for the golden-stand gen_scenarios.py scripts.
//
// Each profile reproduces one Python generator byte for byte. The committed scenarios/*.json
// corpora are the parity reference: running a profile must leave `git status` clean.
//
// Usage: agora_scenarios <profile> <stand_dir>
//   stand_dir is core/tools/golden/<stand>; scenarios are written to <stand>/scenarios.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace fs = std::filesystem;
namespace json = agora::core::json;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << message << "\n";
    std::exit(1);
}

// ---- JSON values ---------------------------------------------------------------------------

json::Value str(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value number(std::int64_t value) { return literal(std::to_string(value)); }

json::Value boolean(bool value) { return literal(value ? "true" : "false"); }

json::Value null_value() { return literal("null"); }

json::Value array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

// ---- serializers (Python json.dump variants) ------------------------------------------------

enum class Format { Compact, Spaced, Pretty2 };

struct FormatOptions {
    Format format = Format::Compact;
    // Python json.dump(ensure_ascii=True) escapes every non-ASCII code point as \uXXXX.
    bool ensure_ascii = false;
};

// Python's json encoder string escaping: quote, backslash, the short control escapes, \u00XX
// for the remaining C0 controls and, when ensure_ascii is set, \uXXXX (surrogate pairs for
// astral code points) for everything above ASCII.
void append_escaped(std::string& out, std::string_view text, bool ensure_ascii) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            switch (byte) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (byte < 0x20) {
                        out += "\\u00";
                        out.push_back(kHex[(byte >> 4) & 0xF]);
                        out.push_back(kHex[byte & 0xF]);
                    } else {
                        out.push_back(static_cast<char>(byte));
                    }
            }
            ++i;
            continue;
        }
        if (!ensure_ascii) {
            out.push_back(static_cast<char>(byte));
            ++i;
            continue;
        }
        std::uint32_t code_point = 0;
        std::size_t length = 0;
        if ((byte & 0xE0) == 0xC0) {
            code_point = byte & 0x1Fu;
            length = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            code_point = byte & 0x0Fu;
            length = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            code_point = byte & 0x07u;
            length = 4;
        } else {
            out += "\\u00";
            out.push_back(kHex[(byte >> 4) & 0xF]);
            out.push_back(kHex[byte & 0xF]);
            ++i;
            continue;
        }
        for (std::size_t k = 1; k < length && i + k < text.size(); ++k) {
            code_point = (code_point << 6) |
                         (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        i += length;
        const auto emit = [&](std::uint32_t unit) {
            out += "\\u";
            out.push_back(kHex[(unit >> 12) & 0xF]);
            out.push_back(kHex[(unit >> 8) & 0xF]);
            out.push_back(kHex[(unit >> 4) & 0xF]);
            out.push_back(kHex[unit & 0xF]);
        };
        if (code_point <= 0xFFFF) {
            emit(code_point);
        } else {
            code_point -= 0x10000;
            emit(0xD800 + (code_point >> 10));
            emit(0xDC00 + (code_point & 0x3FF));
        }
    }
}

void serialize_to(std::string& out, const json::Value& value, const FormatOptions& opts,
                  int depth) {
    const Format format = opts.format;
    switch (value.kind) {
        case json::Kind::String:
            out += '"';
            append_escaped(out, value.text, opts.ensure_ascii);
            out += '"';
            return;
        case json::Kind::Literal:
            out += value.text;
            return;
        case json::Kind::Array: {
            if (value.elements.empty()) {
                out += "[]";
                return;
            }
            out += '[';
            for (std::size_t i = 0; i < value.elements.size(); ++i) {
                if (i > 0) out += (format == Format::Spaced) ? ", " : ",";
                if (format == Format::Pretty2) {
                    out += "\n";
                    out.append(static_cast<std::size_t>((depth + 1) * 2), ' ');
                }
                serialize_to(out, value.elements[i], opts, depth + 1);
            }
            if (format == Format::Pretty2) {
                out += "\n";
                out.append(static_cast<std::size_t>(depth * 2), ' ');
            }
            out += ']';
            return;
        }
        case json::Kind::Object: {
            if (value.members.empty()) {
                out += "{}";
                return;
            }
            out += '{';
            for (std::size_t i = 0; i < value.members.size(); ++i) {
                if (i > 0) out += (format == Format::Spaced) ? ", " : ",";
                if (format == Format::Pretty2) {
                    out += "\n";
                    out.append(static_cast<std::size_t>((depth + 1) * 2), ' ');
                }
                out += '"';
                append_escaped(out, value.members[i].key, opts.ensure_ascii);
                out += '"';
                out += (format == Format::Compact) ? ":" : ": ";
                serialize_to(out, value.members[i].value, opts, depth + 1);
            }
            if (format == Format::Pretty2) {
                out += "\n";
                out.append(static_cast<std::size_t>(depth * 2), ' ');
            }
            out += '}';
            return;
        }
    }
}

std::string serialize(const json::Value& value, const FormatOptions& opts) {
    std::string out;
    serialize_to(out, value, opts, 0);
    return out;
}

void write_scenario(const fs::path& dir, const std::string& name, const json::Value& value,
                    const FormatOptions& opts) {
    std::error_code error;
    fs::create_directories(dir, error);
    if (error) fail("error: cannot create " + dir.string());
    std::ofstream stream(dir / (name + ".json"), std::ios::binary);
    if (!stream) fail("error: cannot write " + (dir / (name + ".json")).string());
    stream << serialize(value, opts) << "\n";
}

void write_scenario(const fs::path& dir, const std::string& name, const json::Value& value,
                    Format format) {
    write_scenario(dir, name, value, FormatOptions{format, false});
}

void remove_stale(const fs::path& dir) {
    std::error_code error;
    if (!fs::is_directory(dir, error)) return;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            fs::remove(entry.path(), error);
        }
    }
}

// ---- profiles --------------------------------------------------------------------------------

// migration-20-35/gen_scenarios.py — default separators (", " / ": ").
void profile_migration_20_35(const fs::path& scenarios) {
    const std::vector<std::pair<std::int64_t, const char*>> names = {
        {20, "m20_21_token_usage_columns"},
        {21, "m21_22_unread_generation"},
        {22, "m22_23_embedding_model_index"},
        {23, "m23_24_new_chat_persist"},
        {24, "m24_25_conversation_settings_transfer"},
        {25, "m25_26_cache_write_tokens"},
        {26, "m26_27_settings_import_transfer"},
        {27, "m27_28_maintenance_debt"},
        {28, "m28_29_semantic_index_tables"},
        {29, "m29_30_reconcile_revision"},
        {30, "m30_31_attachment_scan_index"},
        {31, "m31_32_generation_duration"},
        {32, "m32_33_data_changed_at"},
        {33, "m33_34_task_system_prompt_id"},
        {34, "m34_35_message_source_json"},
    };
    for (const auto& [version, name] : names) {
        write_scenario(scenarios, name,
                       object({{"name", str(name)}, {"op", str("migrate")},
                               {"version", number(version)}}),
                       Format::Spaced);
    }
    std::cout << "wrote " << names.size() << " scenarios into " << scenarios.string() << "\n";
}

// developer-mode-unlock/gen_scenarios.py — compact separators.
void profile_developer_mode_unlock(const fs::path& scenarios) {
    std::vector<json::Value> scenarios_list;
    auto advance = [&](const char* name, std::int64_t current, bool enabled) {
        scenarios_list.push_back(object({{"name", str(name)},
                                         {"op", str("advance")},
                                         {"currentTapCount", number(current)},
                                         {"alreadyEnabled", boolean(enabled)}}));
    };
    auto constants = [&](const char* name) {
        scenarios_list.push_back(object({{"name", str(name)}, {"op", str("constants")}}));
    };

    advance("advance_negative", -5, false);
    advance("advance_int_min", -2147483648LL, false);
    advance("advance_zero", 0, false);
    advance("advance_one", 1, false);
    advance("advance_two", 2, false);
    advance("advance_three", 3, false);
    advance("advance_four", 4, false);
    advance("advance_five", 5, false);
    advance("advance_six_unlocks", 6, false);
    advance("advance_seven_unlocks", 7, false);
    advance("advance_int_max", 2147483647LL, false);

    advance("enabled_zero", 0, true);
    advance("enabled_three", 3, true);
    advance("enabled_six", 6, true);
    advance("enabled_negative", -1, true);

    constants("constants");

    remove_stale(scenarios);
    for (const json::Value& scenario : scenarios_list) {
        std::string name;
        for (const json::Member& member : scenario.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, scenario, Format::Compact);
    }
    std::cout << "wrote " << scenarios_list.size() << " scenarios to " << scenarios.string()
              << "\n";
}

// shell-definitions/gen_scenarios.py — indent=2.
void profile_shell_definitions(const fs::path& scenarios) {
    struct Case {
        const char* name;
        std::vector<std::pair<const char*, const char*>> devices;
        bool sandbox;
        bool shell_enabled;
        std::int64_t tool_timeout_ms;
    };
    const std::vector<Case> cases = {
        {"definitions_shell_disabled", {{"box", "conch"}}, true, false, 60000},
        {"definitions_no_devices_no_sandbox", {}, false, true, 60000},
        {"definitions_no_devices_with_sandbox", {}, true, true, 60000},
        {"definitions_single_conch", {{"box", "conch"}}, false, true, 60000},
        {"definitions_single_ssh", {{"edge", "ssh"}}, false, true, 60000},
        {"definitions_multi_mixed", {{"box", "conch"}, {"edge", "ssh"}}, false, true, 60000},
        {"definitions_multi_conch", {{"box", "conch"}, {"farm", "conch"}}, false, true, 60000},
        {"definitions_sandbox_plus_device", {{"box", "conch"}}, true, true, 60000},
        {"definitions_ssh_only_plus_sandbox", {{"edge", "ssh"}}, true, true, 60000},
        {"definitions_blank_device_name", {{"   ", "conch"}}, false, true, 60000},
        {"definitions_quoted_name_with_comma", {{"a,b \"c\"", "conch"}}, false, true, 60000},
        {"definitions_unknown_device_type", {{"mystery", "telepathy"}}, false, true, 60000},
        {"definitions_timeout_default", {{"box", "conch"}}, false, true, 60000},
        {"definitions_timeout_at_margin", {{"box", "conch"}}, false, true, 5000},
        {"definitions_timeout_below_margin", {{"box", "conch"}}, false, true, 1000},
        {"definitions_timeout_just_above_margin", {{"box", "conch"}}, false, true, 5001},
        {"definitions_timeout_huge", {{"box", "conch"}}, false, true, 2147483647LL},
        {"definitions_timeout_negative", {{"box", "conch"}}, false, true, -5},
    };

    for (const Case& item : cases) {
        std::vector<json::Value> devices;
        for (const auto& [name, type] : item.devices) {
            devices.push_back(object({{"name", str(name)}, {"type", str(type)}}));
        }
        write_scenario(scenarios, item.name,
                       object({{"name", str(item.name)},
                               {"mode", str("definitions")},
                               {"shellEnabled", boolean(item.shell_enabled)},
                               {"sandboxEnabled", boolean(item.sandbox)},
                               {"toolTimeoutMs", number(item.tool_timeout_ms)},
                               {"devices", array(std::move(devices))}}),
                       Format::Pretty2);
    }
    std::cout << "Wrote " << cases.size() << " scenarios\n";
}

// cjk-text/gen_scenarios.py — default separators.
void profile_cjk_text(const fs::path& scenarios) {
    std::vector<std::pair<const char*, std::string>> cases;
    cases.emplace_back("empty", "");
    cases.emplace_back("ascii", "hello world");
    cases.emplace_back("single_han", "\u4e2d");
    cases.emplace_back("han_inside_ascii", "abc\u4e2d\u6587def");
    cases.emplace_back("hiragana_is_excluded", "\u3042");
    cases.emplace_back("katakana_is_excluded", "\u30a2");
    cases.emplace_back("hangul_is_excluded", "\ud55c");
    cases.emplace_back("cjk_punctuation_is_excluded", "\u3002\u3001\u300c\u300d");
    cases.emplace_back("ideographic_zero_is_excluded", "\u3007");
    cases.emplace_back("kanji_iteration_mark_is_excluded", "\u3005");
    cases.emplace_back("fullwidth_latin_is_excluded", "\uff21\uff22\uff23");
    cases.emplace_back("emoji_is_excluded", "\U0001F600");
    cases.emplace_back("ext_a_low_boundary", "\u3400");
    cases.emplace_back("ext_a_high_boundary", "\u4dbf");
    cases.emplace_back("below_ext_a", "\u33ff");
    cases.emplace_back("above_ext_a", "\u4dc0");
    cases.emplace_back("unified_low_boundary", "\u4e00");
    cases.emplace_back("unified_high_boundary", "\u9fff");
    cases.emplace_back("above_unified", "\ua000");
    cases.emplace_back("compat_low_boundary", "\uf900");
    cases.emplace_back("compat_high_boundary", "\ufaff");
    cases.emplace_back("below_compat", "\uf8ff");
    cases.emplace_back("above_compat", "\ufb00");
    cases.emplace_back("supplementary_ext_b_is_ignored", "\U00020000");
    cases.emplace_back("supplementary_compat_is_ignored", "\U0002F800");
    cases.emplace_back("supplementary_not_adjacent_to_range", "\U0002A6D6");
    cases.emplace_back("whitespace_only", " \t\n\u00a0");
    cases.emplace_back("newline_around_han", "\n\u4e2d\n");
    cases.emplace_back("zero_width_space_is_not_han", "\u200b");

    remove_stale(scenarios);
    for (const auto& [name, text] : cases) {
        write_scenario(scenarios, name,
                       object({{"name", str(name)},
                               {"op", str("containsCjk")},
                               {"text", str(text)}}),
                       Format::Spaced);
    }
    std::cout << "wrote " << cases.size() << " scenarios to " << scenarios.string() << "\n";
}

// embedding/gen_scenarios.py — indent=2, optional fields.
void profile_embedding(const fs::path& scenarios) {
    std::vector<json::Value> scenarios_list;
    auto scenario = [&](const char* name, const char* mode,
                        std::vector<std::pair<std::string, json::Value>> extra) {
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("name", str(name));
        members.emplace_back("mode", str(mode));
        for (auto& member : extra) members.push_back(std::move(member));
        scenarios_list.push_back(object(std::move(members)));
    };

    scenario("request_single_special_chars", "requestSingle",
             {{"text", str("Quote \" newline \n backslash \\ euro \u20ac")},
              {"model", str("text-embedding-3-small")}});
    scenario("request_single_custom_model", "requestSingle",
             {{"text", str("hi")}, {"model", str("voyage-3")}});
    scenario("request_batch_special_chars", "requestBatch",
             {{"texts", array({str("first \"quoted\""), str("line\nbreak"), str("tab\there")})},
              {"model", str("text-embedding-3-small")}});
    scenario("request_batch_empty_input", "requestBatch",
             {{"texts", array({})}, {"model", str("m")}});

    scenario("single_ok_extreme_floats", "single",
             {{"response", str("{\"data\":[{\"embedding\":[0.1, -2.5, 1000000.0, 1e-8, -0.0, "
                               "1e999]}]}")}});
    scenario("single_null_response", "single", {{"response", null_value()}});
    scenario("single_missing_response_field", "single", {});
    scenario("single_empty_data", "single", {{"response", str("{\"data\": []}")}});
    scenario("single_missing_data", "single",
             {{"response", str("{\"model\": \"text-embedding-3-small\"}")}});
    scenario("single_data_wrong_kind", "single", {{"response", str("{\"data\": {\"0\": 1}}")}});
    scenario("single_item_not_object", "single", {{"response", str("{\"data\": [\"x\"]}")}});
    scenario("single_embedding_missing", "single", {{"response", str("{\"data\": [{}]}")}});
    scenario("single_embedding_wrong_kind", "single",
             {{"response", str("{\"data\": [{\"embedding\": {}}]}")}});
    scenario("single_element_not_primitive", "single",
             {{"response", str("{\"data\": [{\"embedding\": [{}, 1]}]}")}});
    scenario("single_non_numeric_element", "single",
             {{"response", str("{\"data\": [{\"embedding\": [\"abc\"]}]}")}});
    scenario("single_string_number_elements", "single",
             {{"response", str("{\"data\": [{\"embedding\": [\"1.5\", \"2\"]}]}")}});
    scenario("single_embedding_explicit_null", "single",
             {{"response", str("{\"data\": [{\"embedding\": null}]}")}});
    scenario("single_bad_json", "single", {{"response", str("not-json{")}});
    scenario("single_truncated_json", "single",
             {{"response", str("{\"data\": [{\"embedding\": [1.0")}});

    scenario("batch_ok", "batch",
             {{"texts", array({str("a"), str("b")})},
              {"response", str("{\"data\":[{\"embedding\":[1.0,2.0]},{\"embedding\":[-3.5,4e2]}]}")}});
    scenario("batch_null_response", "batch",
             {{"texts", array({str("a"), str("b")})}, {"response", null_value()}});
    scenario("batch_missing_data", "batch",
             {{"texts", array({str("a"), str("b")})}, {"response", str("{\"ok\": true}")}});
    scenario("batch_item_missing_embedding", "batch",
             {{"texts", array({str("a"), str("b")})},
              {"response", str("{\"data\":[{\"embedding\":[1.0]},{}]}")}});
    scenario("batch_item_not_object", "batch",
             {{"texts", array({str("a"), str("b")})}, {"response", str("{\"data\":[\"x\"]}")}});
    scenario("batch_embedding_explicit_null", "batch",
             {{"texts", array({str("a"), str("b")})},
              {"response", str("{\"data\":[{\"embedding\":[1.0]},{\"embedding\":null}]}")}});
    scenario("batch_element_not_primitive", "batch",
             {{"texts", array({str("a")})},
              {"response", str("{\"data\":[{\"embedding\":[{\"nested\":1}]}]}")}});
    scenario("batch_count_mismatch_follows_response", "batch",
             {{"texts", array({str("a"), str("b"), str("c")})},
              {"response", str("{\"data\":[{\"embedding\":[1.0]},{\"embedding\":[2.0]}]}")}});
    scenario("batch_empty_texts_skips_response", "batch",
             {{"texts", array({})}, {"response", null_value()}});
    scenario("batch_bad_json", "batch",
             {{"texts", array({str("a")})}, {"response", str("<html>error</html>")}});

    for (const json::Value& item : scenarios_list) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, Format::Pretty2);
    }
    std::cout << "Wrote " << scenarios_list.size() << " scenarios\n";
}

// native-backup-format/gen_scenarios.py — indent=2 with nested settings.
void profile_native_backup_format(const fs::path& scenarios) {
    auto shell = [](const std::string& api_key, const std::string& ssh_password) {
        return object({{"id", str("s1")},
                       {"name", str("dev")},
                       {"description", str("d")},
                       {"type", str("ssh")},
                       {"serverUrl", str("https://conch.example")},
                       {"apiKey", str(api_key)},
                       {"conchPublicKey", str("pub")},
                       {"sshHost", str("host")},
                       {"sshPort", number(2222)},
                       {"sshUser", str("root")},
                       {"sshPassword", str(ssh_password)},
                       {"sshHostKey", str("key")}});
    };
    auto mcp = [](std::vector<std::pair<std::string, json::Value>> headers) {
        return object({{"id", str("m1")},
                       {"name", str("server")},
                       {"headers", object(std::move(headers))}});
    };
    auto embedding = [](const std::string& kind, const std::string& remote_key,
                        const std::string& local_path) {
        return object({{"id", str("e1")},
                       {"name", str("model")},
                       {"type", str(kind)},
                       {"remoteModelName", str("text-embedding")},
                       {"remoteBaseUrl", str("https://api.example")},
                       {"remoteApiKey", str(remote_key)},
                       {"localFilePath", str(local_path)},
                       {"batchSize", number(16)}});
    };

    std::vector<json::Value> cases;
    cases.push_back(object({{"name", str("format_constants")}, {"op", str("format")}}));
    cases.push_back(object({{"name", str("conversation_entries")},
                            {"op", str("entry")},
                            {"ids", array({str("plain"), str("conv 1"), str("a/b"),
                                           str("\u041f\u0440\u0438\u0432\u0435\u0442"),
                                           str("\U0001F600"), str("tab\tname"), str("")})}}));
    cases.push_back(object({{"name", str("models_remote")},
                            {"op", str("models")},
                            {"settings", object({{"shell", shell("secret-key", "secret-pw")},
                                                 {"mcp", mcp({{"Authorization", str("Bearer x")},
                                                              {"X-Empty", str("")}})},
                                                 {"embedding", embedding("REMOTE", "embed-key",
                                                                         "/tmp/model.bin")}})}}));
    cases.push_back(object({{"name", str("models_local")},
                            {"op", str("models")},
                            {"settings", object({{"shell", shell("", "")},
                                                 {"mcp", mcp({})},
                                                 {"embedding", embedding("LOCAL", "ignored-key",
                                                                         "/tmp/local.bin")}})}}));
    cases.push_back(object({{"name", str("models_remote_blank")},
                            {"op", str("models")},
                            {"settings", object({{"shell", shell("", "pw")},
                                                 {"mcp", mcp({{"A", str("B")}})},
                                                 {"embedding", embedding("REMOTE", "", "")}})}}));

    for (const json::Value& item : cases) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, Format::Pretty2);
    }
    std::cout << "wrote " << cases.size() << " scenarios\n";
}


// malformed-tool-call/gen_scenarios.py — indent=2, ensure_ascii=False, directory recreated.
void profile_malformed_tool_call(const fs::path& scenarios) {
    auto request = [](std::string cause, json::Value original_name, json::Value original_arguments,
                      json::Value stream_key, json::Value signature) {
        return object({{"cause", str(std::move(cause))},
                       {"originalName", std::move(original_name)},
                       {"originalArguments", std::move(original_arguments)},
                       {"streamKey", std::move(stream_key)},
                       {"signature", std::move(signature)}});
    };
    auto scenario = [](json::Value requests, json::Value results) {
        return object({{"requests", std::move(requests)}, {"results", std::move(results)}});
    };
    const FormatOptions pretty{Format::Pretty2, false};

    remove_stale(scenarios);
    write_scenario(scenarios, "01-malformed-basic",
                   scenario(array({request("the tool name was empty", null_value(), null_value(),
                                          null_value(), null_value())}),
                            array({str("{\"error\":\"the tool name was empty\"}")})),
                   pretty);
    write_scenario(scenarios, "02-malformed-with-originals",
                   scenario(array({request("duplicate tool call id", str("search"),
                                          str("{\"q\":\"x\"}"), str("stream-7"), str("sig-1"))}),
                            array({str("{\"error\":\"duplicate tool call id\",\"original_name\":"
                                       "\"search\",\"original_arguments\":\"{\\\"q\\\":\\\"x\\\"}\"}")})),
                   pretty);
    write_scenario(scenarios, "03-malformed-blank-originals",
                   scenario(array({request("arguments were not an object", str(""), str(""),
                                          str("  "), str(""))}),
                            array({str("{\"error\":\"arguments were not an object\"}")})),
                   pretty);
    const std::string long_cause(500, 'x');
    const std::string long_name(500, 'n');
    const std::string long_arguments(5000, 'a');
    write_scenario(scenarios, "04-malformed-long-originals",
                   scenario(array({request(long_cause, str(long_name), str(long_arguments),
                                          null_value(), null_value())}),
                            array({str("{\"error\":\"" + long_cause + "\",\"original_name\":\"" +
                                       long_name + "\",\"original_arguments\":\"" +
                                       long_arguments + "\"}")})),
                   pretty);
    write_scenario(scenarios, "05-malformed-result-texts",
                   scenario(array({request("cause", null_value(), null_value(), null_value(),
                                          null_value())}),
                            array({str("not json at all"), str(""), str("{}"),
                                   str("{\"error\":\"\"}"),
                                   str("{\"error\":\"e\",\"original_name\":\"\","
                                       "\"original_arguments\":\"\"}"),
                                   str("{\"error\":42}"), str("{\"other\":\"x\"}")})),
                   pretty);
    std::cout << "Wrote 5 scenarios to " << scenarios.string() << "\n";
}

// provider-names/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_provider_names(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto allowed = [&](const char* name, const char* provider) {
        all.push_back(object(
            {{"name", str(name)}, {"mode", str("allowed")}, {"providerName", str(provider)}}));
    };
    auto conflict = [&](const char* name, const char* provider,
                        std::vector<std::string> existing, const char* current) {
        std::vector<json::Value> names;
        for (auto& item : existing) names.push_back(str(std::move(item)));
        std::vector<std::pair<std::string, json::Value>> members = {
            {"name", str(name)},
            {"mode", str("conflict")},
            {"providerName", str(provider)},
            {"existingNames", array(std::move(names))},
        };
        if (current != nullptr) members.emplace_back("currentName", str(current));
        all.push_back(object(std::move(members)));
    };
    auto sanitize = [&](const char* name, std::vector<std::string> configs) {
        std::vector<json::Value> items;
        for (auto& item : configs) items.push_back(str(std::move(item)));
        all.push_back(object({{"name", str(name)},
                              {"mode", str("sanitize")},
                              {"configs", array(std::move(items))}}));
    };

    allowed("allowed_plain", "My Provider");
    allowed("allowed_trimmed_padded", "  My Provider  ");
    allowed("allowed_empty", "");
    allowed("allowed_blank", "   ");
    allowed("allowed_reserved_google", "Google");
    allowed("allowed_reserved_lowercase", "google");
    allowed("allowed_reserved_upper", "GOOGLE");
    allowed("allowed_reserved_padded", "  Anthropic ");
    allowed("allowed_reserved_openrouter", "open router");
    allowed("allowed_reserved_opencode_go", "OpenCode Go");
    allowed("allowed_reserved_local", "Local");
    allowed("allowed_reserved_deepseek", "DeepSeek");
    allowed("allowed_reserved_qwen", "Qwen");
    allowed("allowed_reserved_groq", "Groq");
    allowed("allowed_reserved_ollama", "Ollama");
    allowed("allowed_reserved_openai", "OpenAI");
    allowed("allowed_similar_to_reserved", "Googler");
    allowed("allowed_reserved_with_inner_space", "Deep  Seek");
    allowed("allowed_unicode", "\u041f\u0440\u043e\u0432\u0430\u0439\u0434\u0435\u0440 \U0001F680");

    conflict("conflict_duplicate_case_insensitive", "Box", {"box"}, nullptr);
    conflict("conflict_duplicate_trimmed", " Box ", {"Box"}, nullptr);
    conflict("conflict_distinct", "Box", {"Bax"}, nullptr);
    conflict("conflict_no_existing", "Box", {}, nullptr);
    conflict("conflict_disallowed_wins", "Google", {}, nullptr);
    conflict("conflict_disallowed_with_existing", "Google", {"Google Custom"}, nullptr);
    conflict("conflict_current_entry_exempt", "Box", {"Box"}, "Box");
    conflict("conflict_current_entry_case_sensitive", "Box", {"Box"}, "box");
    conflict("conflict_current_entry_not_first", "Box", {"Other", "Box"}, "Box");
    conflict("conflict_current_entry_absent", "Box", {"Box"}, nullptr);
    conflict("conflict_current_entry_untrimmed", "Box", {" Box "}, " Box ");
    conflict("conflict_blank_candidate", "   ", {"x"}, nullptr);
    conflict("conflict_multiple_duplicates", "Box", {"box", "BOX"}, nullptr);

    sanitize("sanitize_empty", {});
    sanitize("sanitize_single", {"Box"});
    sanitize("sanitize_first_wins", {"Box", "box", " Google", "Local", "Foo"});
    sanitize("sanitize_all_rejected", {"Google", "Local", ""});
    sanitize("sanitize_all_accepted", {"Alpha", "Beta", "Gamma"});
    sanitize("sanitize_trimmed_duplicates", {"Box", "  Box  ", "\tBox\n"});
    sanitize("sanitize_blank_first", {"   ", "Box"});

    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "Wrote " << all.size() << " scenarios\n";
}

// shell-command-validation/gen_scenarios.py — indent=2, ensure_ascii=True (Python default).
void profile_shell_command_validation(const fs::path& scenarios) {
    const FormatOptions pretty{Format::Pretty2, true};
    remove_stale(scenarios);
    auto write = [&](const char* name, const std::string& command, const std::string& workdir) {
        write_scenario(scenarios, name,
                       object({{"op", str("shellCommandValidationError")},
                               {"command", str(command)},
                               {"workdir", str(workdir)}}),
                       pretty);
    };
    write("01-valid_small", "ls -la", "/tmp");
    write("02-command_at_limit", std::string(65536, 'x'), "/tmp");
    write("03-command_over_limit", std::string(65537, 'x'), "/tmp");
    write("04-workdir_at_limit", "ls", std::string(32768, 'x'));
    write("05-workdir_over_limit", "ls", std::string(32769, 'x'));
    // Build the euro-sign command by hand: 21846 copies of U+20AC.
    {
        std::string command = "echo ";
        for (int i = 0; i < 21846; ++i) command += "\u20ac";
        write("06-command_multibyte_utf8", command, "/tmp");
    }
    write("07-command_with_newline", "echo hello\necho world", "/tmp");
    {
        std::string workdir;
        for (int i = 0; i < 10923; ++i) workdir += "\u20ac";
        write("08-workdir_multibyte_utf8", "ls", workdir);
    }
    write("09-empty_command", "", "/tmp");
    write("10_both_over_limit", std::string(65537, 'x'), std::string(32769, 'y'));
    std::cout << "Wrote 10 scenarios\n";
}

// database-compatibility/gen_scenarios.py — compact, ensure_ascii=False.
void profile_database_compatibility(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto classify = [&](const char* name, bool exists, std::int64_t current,
                        std::optional<std::int64_t> stored, const char* throw_class,
                        const char* throw_message) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"name", str(name)},
            {"op", str("classify")},
            {"databaseExists", boolean(exists)},
            {"currentVersion", number(current)},
        };
        if (stored) members.emplace_back("storedVersion", number(*stored));
        if (throw_class != nullptr) members.emplace_back("throwClass", str(throw_class));
        if (throw_message != nullptr) members.emplace_back("throwMessage", str(throw_message));
        all.push_back(object(std::move(members)));
    };
    auto can_open = [&](const char* name, bool exists, std::int64_t current,
                        std::optional<std::int64_t> stored, const char* throw_class,
                        const char* throw_message) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"name", str(name)},
            {"op", str("can_open")},
            {"databaseExists", boolean(exists)},
            {"currentVersion", number(current)},
        };
        if (stored) members.emplace_back("storedVersion", number(*stored));
        if (throw_class != nullptr) members.emplace_back("throwClass", str(throw_class));
        if (throw_message != nullptr) members.emplace_back("throwMessage", str(throw_message));
        all.push_back(object(std::move(members)));
    };

    classify("classify_missing", false, 5, 0, nullptr, nullptr);
    classify("classify_missing_ignores_reader", false, 5, std::nullopt,
             "java.lang.IllegalStateException", "boom");
    classify("classify_supported_older", true, 5, 4, nullptr, nullptr);
    classify("classify_supported_equal", true, 5, 5, nullptr, nullptr);
    classify("classify_supported_zero", true, 0, 0, nullptr, nullptr);
    classify("classify_supported_negative_stored", true, 5, -1, nullptr, nullptr);
    classify("classify_supported_int_min", true, 0, -2147483648LL, nullptr, nullptr);
    classify("classify_future_one_ahead", true, 5, 6, nullptr, nullptr);
    classify("classify_future_far", true, 5, 99, nullptr, nullptr);
    classify("classify_future_negative_current", true, -1, 0, nullptr, nullptr);
    classify("classify_future_int_extremes", true, -2147483648LL, 2147483647LL, nullptr,
             nullptr);

    classify("classify_unreadable_ise", true, 5, std::nullopt,
             "java.lang.IllegalStateException", "boom");
    classify("classify_unreadable_ise_no_message", true, 5, std::nullopt,
             "java.lang.IllegalStateException", nullptr);
    classify("classify_unreadable_ise_empty_message", true, 5, std::nullopt,
             "java.lang.IllegalStateException", "");
    classify("classify_unreadable_npe", true, 5, std::nullopt,
             "java.lang.NullPointerException", "x is null");
    classify("classify_unreadable_runtime", true, 5, std::nullopt,
             "java.lang.RuntimeException", "db locked");
    classify("classify_unreadable_io", true, 5, std::nullopt, "java.io.IOException", "disk");
    classify("classify_unreadable_argument", true, 5, std::nullopt,
             "java.lang.IllegalArgumentException", "version");

    can_open("can_open_missing", false, 5, 0, nullptr, nullptr);
    can_open("can_open_supported", true, 5, 4, nullptr, nullptr);
    can_open("can_open_supported_negative_stored", true, 5, -1, nullptr, nullptr);
    can_open("can_open_future", true, 5, 6, nullptr, nullptr);
    can_open("can_open_unreadable", true, 5, std::nullopt,
             "java.lang.IllegalStateException", "boom");

    remove_stale(scenarios);
    const FormatOptions compact{Format::Compact, false};
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, compact);
    }
    std::cout << "wrote " << all.size() << " scenarios to " << scenarios.string() << "\n";
}

// context-budget/gen_scenarios.py — compact, ensure_ascii=False.
void profile_context_budget(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto normalize = [&](const char* name, json::Value value) {
        all.push_back(object(
            {{"name", str(name)}, {"op", str("normalize")}, {"value", std::move(value)}}));
    };
    auto label = [&](const char* name, std::int64_t tokens) {
        all.push_back(object(
            {{"name", str(name)}, {"op", str("label")}, {"tokens", number(tokens)}}));
    };
    auto presets = [&](const char* name) {
        all.push_back(object({{"name", str(name)}, {"op", str("presets")}}));
    };

    normalize("normalize_null", null_value());
    normalize("normalize_zero", number(0));
    normalize("normalize_negative", number(-1));
    normalize("normalize_one", number(1));
    normalize("normalize_four", number(4));
    normalize("normalize_fifty", number(50));
    normalize("normalize_sixty_four", number(64));
    normalize("normalize_twenty", number(20));
    normalize("normalize_hundred", number(100));
    normalize("normalize_above_legacy", number(101));
    normalize("normalize_thousand", number(1000));
    normalize("normalize_min", number(4095));
    normalize("normalize_exact_min", number(4096));
    normalize("normalize_8192", number(8192));
    normalize("normalize_200000", number(200000));
    normalize("normalize_exact_max", number(1048576));
    normalize("normalize_over_max", number(2000000));
    normalize("normalize_int_max", number(2147483647LL));

    label("label_zero", 0);
    label("label_negative", -1);
    label("label_negative_large", -1000);
    label("label_one", 1);
    label("label_999", 999);
    label("label_1000", 1000);
    label("label_1001", 1001);
    label("label_1024", 1024);
    label("label_1536", 1536);
    label("label_2048", 2048);
    label("label_4095", 4095);
    label("label_4096", 4096);
    label("label_4097", 4097);
    label("label_4198", 4198);
    label("label_8192", 8192);
    label("label_65536", 65536);
    label("label_262144", 262144);
    label("label_524288", 524288);
    label("label_1000000", 1000000);
    label("label_1048575", 1048575);
    label("label_1048576", 1048576);
    label("label_1048577", 1048577);
    label("label_1572864", 1572864);
    label("label_5242880", 5242880);
    label("label_int_max", 2147483647LL);
    label("label_123456789", 123456789);

    presets("presets_list");

    remove_stale(scenarios);
    const FormatOptions compact{Format::Compact, false};
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, compact);
    }
    std::cout << "wrote " << all.size() << " scenarios to " << scenarios.string() << "\n";
}

// memory-manager/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_memory_manager(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto add = [&](const char* name, const char* mode,
                   std::vector<std::pair<std::string, json::Value>> extra) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)},
                                                                    {"mode", str(mode)}};
        for (auto& member : extra) members.push_back(std::move(member));
        all.push_back(object(std::move(members)));
    };

    add("active_replace", "active", {{"content_param", str("Hello world")}});
    add("active_append", "active",
        {{"active_seed", str("First line")},
         {"content_param", str("Second line")},
         {"mode_param", str("append")}});
    add("active_prepend", "active",
        {{"active_seed", str("Last line")},
         {"content_param", str("First line")},
         {"mode_param", str("prepend")}});
    add("active_patch", "active",
        {{"active_seed", str("Hello world")},
         {"mode_param", str("patch")},
         {"old_string", str("world")},
         {"new_string", str("universe")}});
    add("active_patch_not_found", "active",
        {{"active_seed", str("Hello world")},
         {"mode_param", str("patch")},
         {"old_string", str("missing")},
         {"new_string", str("found")}});
    add("active_patch_multiple", "active",
        {{"active_seed", str("foo bar foo")},
         {"mode_param", str("patch")},
         {"old_string", str("foo")},
         {"new_string", str("baz")}});
    add("active_patch_no_old_string", "active",
        {{"active_seed", str("Hello world")}, {"mode_param", str("patch")}});

    add("description_get_missing", "description_get", {{"file_name", str("missing.md")}});
    add("description_set", "description_set",
        {{"file_name", str("test.md")},
         {"setup_content", str("Original")},
         {"description", str("A test file")}});
    add("description_set_blank_removes", "description_set",
        {{"file_name", str("test.md")},
         {"setup_content", str("Original")},
         {"description", str("")}});

    add("list_empty", "list", {});
    add("list_sorted", "list",
        {{"file_name", str("b.md")}, {"setup_content", str("b")}});

    add("meta_empty", "meta", {});
    add("meta_save_restore", "meta_save",
        {{"meta_json", str("{\"file1.md\": \"desc1\", \"file2.md\": \"desc2\"}")}});

    add("read_missing", "read", {{"file_name", str("missing.md")}});
    add("read_existing", "read",
        {{"file_name", str("test.md")}, {"setup_content", str("Hello world")}});

    add("create_basic", "create",
        {{"file_name", str("new.md")}, {"content_param", str("# Hello")}});
    add("create_with_description", "create",
        {{"file_name", str("test.md")},
         {"content_param", str("Content")},
         {"description", str("A test file")}});
    add("create_existing", "create",
        {{"file_name", str("existing.md")},
         {"setup_content", str("Original")},
         {"content_param", str("Duplicate")}});

    add("edit_replace", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("Original")},
         {"content_param", str("New content")}});
    add("edit_rename", "edit",
        {{"file_name", str("old.md")},
         {"setup_content", str("Original")},
         {"new_name", str("renamed.md")}});
    add("edit_rename_and_content", "edit",
        {{"file_name", str("old.md")},
         {"setup_content", str("Original")},
         {"content_param", str("New content")},
         {"new_name", str("renamed.md")}});
    add("edit_description", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("Original")},
         {"description", str("New description")}});
    add("edit_patch", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("Hello old world")},
         {"old_string", str("old")},
         {"new_string", str("new")}});
    add("edit_patch_not_found", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("Hello world")},
         {"old_string", str("missing")},
         {"new_string", str("found")}});
    add("edit_patch_multiple", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("foo bar foo")},
         {"old_string", str("foo")},
         {"new_string", str("bar")}});
    add("edit_patch_no_match", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("Hello world")},
         {"old_string", str("")},
         {"new_string", str("new")}});
    add("edit_rename_conflict", "edit",
        {{"file_name", str("test.md")},
         {"setup_content", str("Original")},
         {"new_name", str("existing.md")},
         {"setup_target_name", str("existing.md")},
         {"setup_target_content", str("Target")}});
    add("edit_no_changes", "edit",
        {{"file_name", str("test.md")}, {"setup_content", str("Original")}});

    add("delete_existing", "delete",
        {{"file_name", str("test.md")}, {"setup_content", str("Content to delete")}});
    add("delete_missing", "delete", {{"file_name", str("missing.md")}});

    add("revision_check", "revision", {});

    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "Wrote " << all.size() << " scenarios\n";
}


// provider-support/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_provider_support(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto http_error = [&](const char* name, std::int64_t status, json::Value body) {
        all.push_back(object({{"name", str(name)},
                              {"mode", str("httpError")},
                              {"statusCode", number(status)},
                              {"rawBody", std::move(body)}}));
    };
    auto base_url = [&](const char* name, const char* url) {
        all.push_back(object(
            {{"name", str(name)}, {"mode", str("baseUrl")}, {"url", str(url)}}));
    };
    auto defaults = [&](const char* name, const char* embedding, const char* compatible,
                        std::vector<std::pair<std::string, json::Value>> openai_map) {
        all.push_back(object({{"name", str(name)},
                              {"mode", str("defaults")},
                              {"embeddingProvider", str(embedding)},
                              {"compatibleName", str(compatible)},
                              {"openAiMap", object(std::move(openai_map))}}));
    };

    http_error("http_null_body", 404, null_value());
    http_error("http_blank_body", 404, str("   "));
    http_error("http_free_text", 503, str("upstream exploded"));
    http_error("http_primitive_string", 429, str("\"quota exceeded\""));
    http_error("http_primitive_blank_string", 500, str("\"   \""));
    http_error("http_primitive_number", 502, str("42"));
    http_error("http_primitive_true", 500, str("true"));
    http_error("http_json_null_literal", 500, str("null"));
    http_error("http_array_root", 500, str("[1, 2]"));
    http_error("http_object_nested_full", 429,
               str("{\"error\": {\"code\": \"rate_limit\", \"type\": \"overloaded\", "
                   "\"message\": \"slow down\"}}"));
    http_error("http_object_nested_status_detail", 401,
               str("{\"error\": {\"status\": \"InvalidApiKey\", \"detail\": \"bad key\"}}"));
    http_error("http_object_error_string", 500, str("{\"error\": \"quota hit\"}"));
    http_error("http_object_root_message", 500, str("{\"message\": \"root msg\"}"));
    http_error("http_object_root_detail", 500, str("{\"detail\": \"root detail\"}"));
    http_error("http_object_root_reason", 500, str("{\"reason\": \"root reason\"}"));
    http_error("http_object_root_error_description", 500,
               str("{\"error_description\": \"oauth failed\"}"));
    http_error("http_object_no_message", 500, str("{\"code\": 500}"));
    http_error("http_nested_number_code", 429,
               str("{\"error\": {\"code\": 429, \"message\": \"rl\"}}"));
    http_error("http_nested_trimmed_message", 500,
               str("{\"error\": {\"message\": \"  padded  \"}}"));
    http_error("http_nested_null_message_falls_back", 500,
               str("{\"error\": {\"message\": null, \"detail\": \"detail wins\"}}"));
    http_error("http_status_fallback_code", 502,
               str("{\"error\": {\"message\": \"bad gateway\"}}"));
    http_error("http_quoted_null_string_message", 500, str("{\"error\": \"null\"}"));

    base_url("base_url_plain", "https://api.example.com");
    base_url("base_url_blank", "");
    base_url("base_url_slash_only", "/");
    base_url("base_url_version", "https://api.example.com/v1");
    base_url("base_url_version_trailing_slash", "https://api.example.com/v1/");
    base_url("base_url_version_multislash", "https://api.example.com/v1///");
    base_url("base_url_version_beta", "https://api.example.com/v1beta");
    base_url("base_url_version_midpath", "https://api.example.com/v1/proxy");
    base_url("base_url_version_v2", "https://api.example.com/v2");
    base_url("base_url_version_upper", "https://api.example.com/V1BETA");
    base_url("base_url_no_slash_prefix", "v1");
    base_url("base_url_only_version", "/v1");
    base_url("base_url_double_slash_v", "https://v1.example.com/x");
    base_url("base_url_version_trailing_newline", "https://api.example.com/v1beta\n");
    base_url("base_url_digit_run", "https://api.example.com/v1x_9");

    defaults("defaults_openai", "OpenAI", "OpenAI",
             {{"OpenAI", str("https://custom.openai/v1")}});
    defaults("defaults_openai_empty_map", "openai", "openai", {});
    defaults("defaults_openai_empty_value", "", "OpenAI", {{"OpenAI", str("")}});
    defaults("defaults_openai_other_key", "", "Google",
             {{"Google", str("https://elsewhere/v1")}});
    defaults("defaults_open_router", "open router", "Open Router", {});
    defaults("defaults_openrouter_compact", "OpenRouter", "", {});
    defaults("defaults_mistral_upper", "MISTRAL", "", {});
    defaults("defaults_voyage_variants", "voyage ai", "", {});
    defaults("defaults_voyage_compact", "VOYAGE", "", {});
    defaults("defaults_siliconflow", "siliconflow", "", {});
    defaults("defaults_ollama", "ollama", "", {});
    defaults("defaults_unknown", "deepseek", "", {});

    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "Wrote " << all.size() << " scenarios\n";
}

// run-branch-selection/gen_scenarios.py — default separators.
void profile_run_branch_selection(const fs::path& scenarios) {
    struct Selection {
        const char* parent;
        const char* child;
    };
    struct Run {
        const char* id;
        const char* parent;
    };
    struct Case {
        const char* name;
        std::vector<Selection> selections;
        std::vector<Run> runs;
    };
    const std::vector<Case> cases = {
        {"keeps_valid_edge", {{"p", "c"}}, {{"p", nullptr}, {"c", "p"}}},
        {"keeps_root_child", {{nullptr, "c"}}, {{"c", nullptr}}},
        {"chain_keeps_every_valid_edge",
         {{nullptr, "root"}, {"root", "a"}, {"a", "b"}},
         {{"root", nullptr}, {"a", "root"}, {"b", "a"}}},
        {"drops_missing_child", {{"p", "c"}}, {{"p", nullptr}}},
        {"drops_root_selection_for_missing_child", {{nullptr, "c"}}, {}},
        {"drops_root_child_that_has_a_parent", {{nullptr, "c"}}, {{"c", "p"}}},
        {"drops_non_root_selection_for_root_child", {{"p", "c"}},
         {{"p", nullptr}, {"c", nullptr}}},
        {"drops_child_with_another_parent", {{"p", "c"}},
         {{"p", nullptr}, {"q", nullptr}, {"c", "q"}}},
        {"child_repeated_under_two_parents", {{"p", "c"}, {"q", "c"}},
         {{"p", nullptr}, {"q", nullptr}, {"c", "p"}}},
        {"drops_missing_parent", {{"p", "c"}}, {{"c", "p"}}},
        {"drops_self_edge", {{"c", "c"}}, {{"c", "c"}}},
        {"drops_self_edge_for_root_child", {{"c", "c"}}, {{"c", nullptr}}},
        {"empty_selections", {}, {{"a", nullptr}}},
        {"empty_runs", {{"p", "c"}}, {}},
        {"preserves_selection_order",
         {{"p1", "c1"}, {nullptr, "c3"}, {"p2", "c2"}},
         {{"p1", nullptr}, {"c1", "p1"}, {"p2", nullptr}, {"c2", "p2"}, {"c3", nullptr}}},
        {"keeps_valid_and_drops_invalid_in_order",
         {{nullptr, "root"}, {"p", "c"}, {"q", "c"}, {nullptr, "missing"}},
         {{"root", nullptr}, {"p", nullptr}, {"q", nullptr}, {"c", "p"}}},
        {"duplicate_run_ids_last_wins", {{"p1", "c"}, {"p2", "c"}},
         {{"p1", nullptr}, {"p2", nullptr}, {"c", "p1"}, {"c", "p2"}}},
        {"duplicate_run_ids_last_is_root", {{nullptr, "c"}},
         {{"c", "p"}, {"c", nullptr}}},
        {"duplicate_run_ids_last_has_parent", {{nullptr, "c"}},
         {{"c", nullptr}, {"c", "p"}}},
        {"duplicate_run_ids_last_root_drops_parent_edge",
         {{nullptr, "x"}, {"p2", "x"}},
         {{"x", "p1"}, {"x", "p2"}, {"x", nullptr}}},
        {"duplicate_selection_key_last_value_wins", {{"p1", "c1"}, {"p1", "c2"}},
         {{"p1", nullptr}, {"c1", "p1"}, {"c2", "p1"}}},
        {"duplicate_null_key_last_value_wins", {{nullptr, "c1"}, {nullptr, "c2"}},
         {{"c1", nullptr}, {"c2", nullptr}}},
        {"empty_string_parent_id", {{"", "c"}}, {{"", nullptr}, {"c", ""}}},
        {"empty_string_child_id", {{"p", ""}}, {{"p", nullptr}, {"", "p"}}},
        {"null_parent_is_not_empty_string", {{nullptr, "c"}}, {{"c", ""}}},
        {"separators_are_escaped", {{"a=b|c;d", "c"}},
         {{"a=b|c;d", nullptr}, {"c", "a=b|c;d"}}},
        {"unrelated_runs_are_ignored", {{"p", "c"}},
         {{"x", nullptr}, {"y", "x"}, {"p", nullptr}, {"c", "p"}}},
    };

    remove_stale(scenarios);
    for (const Case& item : cases) {
        std::vector<json::Value> selections;
        for (const Selection& selection : item.selections) {
            selections.push_back(array({selection.parent == nullptr ? null_value()
                                                                    : str(selection.parent),
                                        str(selection.child)}));
        }
        std::vector<json::Value> runs;
        for (const Run& run : item.runs) {
            runs.push_back(object({{"id", str(run.id)},
                                   {"parentRunId", run.parent == nullptr
                                                       ? null_value()
                                                       : str(run.parent)}}));
        }
        write_scenario(scenarios, item.name,
                       object({{"name", str(item.name)},
                               {"op", str("retainValidEdges")},
                               {"selections", array(std::move(selections))},
                               {"runs", array(std::move(runs))}}),
                       Format::Spaced);
    }
    std::cout << "wrote " << cases.size() << " scenarios to " << scenarios.string() << "\n";
}

// custom-model-references/gen_scenarios.py — default separators.
void profile_custom_model_references(const fs::path& scenarios) {
    const std::string nbsp = "\u00a0";
    const std::string zwsp = "\u200b";
    std::vector<json::Value> all;
    auto case_set = [&](const char* name, const char* old_model, json::Value new_model,
                        std::vector<std::string> items) {
        std::vector<json::Value> values;
        for (auto& item : items) values.push_back(str(std::move(item)));
        all.push_back(object({{"name", str(name)},
                              {"op", str("set")},
                              {"oldModelId", str(old_model)},
                              {"newModelId", std::move(new_model)},
                              {"items", array(std::move(values))}}));
    };
    auto case_string = [&](const char* name, json::Value value, const char* old_model,
                           json::Value new_model) {
        all.push_back(object({{"name", str(name)},
                              {"op", str("string")},
                              {"value", std::move(value)},
                              {"oldModelId", str(old_model)},
                              {"newModelId", std::move(new_model)}}));
    };
    auto case_map = [&](const char* name, const char* old_model, json::Value new_model,
                        const std::string& alias,
                        std::vector<std::pair<std::string, std::string>> entries) {
        std::vector<json::Value> pairs;
        for (const auto& [key, value] : entries) {
            pairs.push_back(array({str(key), str(value)}));
        }
        all.push_back(object({{"name", str(name)},
                              {"op", str("map")},
                              {"oldModelId", str(old_model)},
                              {"newModelId", std::move(new_model)},
                              {"alias", str(alias)},
                              {"entries", array(std::move(pairs))}}));
    };

    case_set("set_replaces_existing", "a", str("b"), {"a", "c"});
    case_set("set_removes_when_new_is_null", "a", null_value(), {"a", "b"});
    case_set("set_absent_old_is_unchanged", "x", str("y"), {"a", "b"});
    case_set("set_absent_old_with_null_new_is_unchanged", "x", null_value(), {"a", "b"});
    case_set("set_old_equals_new_keeps_place", "a", str("a"), {"a", "b"});
    case_set("set_new_already_present_dedupes", "a", str("c"), {"a", "c"});
    case_set("set_new_present_before_old_dedupes", "a", str("c"), {"c", "a"});
    case_set("set_removes_only_element", "a", null_value(), {"a"});
    case_set("set_empty_input", "a", str("b"), {});
    case_set("set_empty_old_matches_empty_element", "", str("x"), {"", "a"});
    case_set("set_new_is_empty_string", "a", str(""), {"a", "b"});
    case_set("set_duplicate_input_entries_dedupe", "x", str("y"), {"a", "a", "b"});
    case_set("set_unicode_ids", "\u043c\u043e\u0434\u0435\u043b\u044c",
             str("\u043c\u043e\u0434\u0435\u043b\u044c2"),
             {"\u043c\u043e\u0434\u0435\u043b\u044c", "b"});
    case_set("set_separators_are_escaped", "a=b|c;d", str("x=y"), {"a=b|c;d", "e"});

    case_string("string_null_stays_null", null_value(), "a", str("b"));
    case_string("string_match_replaces", str("a"), "a", str("b"));
    case_string("string_match_to_null", str("a"), "a", null_value());
    case_string("string_no_match_is_unchanged", str("b"), "a", str("c"));
    case_string("string_old_equals_new", str("a"), "a", str("a"));
    case_string("string_empty_matches_empty", str(""), "", str("x"));
    case_string("string_empty_value_other_old", str(""), "a", str("x"));
    case_string("string_separators_are_escaped", str("a|b;c"), "a|b;c", str("d|e;f"));

    case_map("map_renames_to_absent_key_appends", "a", str("b"), "x", {{"a", "1"}});
    case_map("map_replaces_value_in_place", "a", str("c"), "x", {{"a", "1"}, {"b", "2"}});
    case_map("map_removes_when_new_is_null", "a", null_value(), "x", {{"a", "1"}, {"b", "2"}});
    case_map("map_old_equals_new_moves_to_end", "a", str("a"), "3", {{"a", "1"}, {"b", "2"}});
    case_map("map_existing_target_value_overwritten_in_place", "a", str("b"), "3",
             {{"a", "1"}, {"b", "2"}});
    case_map("map_existing_target_before_old", "a", str("b"), "3", {{"b", "2"}, {"a", "1"}});
    case_map("map_blank_alias_removes_only", "a", str("b"), "", {{"a", "1"}});
    case_map("map_space_alias_is_blank", "a", str("b"), "   ", {{"a", "1"}});
    case_map("map_nbsp_alias_is_blank", "a", str("b"), nbsp, {{"a", "1"}});
    case_map("map_zwsp_alias_is_content", "a", str("b"), zwsp, {{"a", "1"}});
    case_map("map_nbsp_padded_alias_is_trimmed", "a", str("b"), nbsp + "x" + nbsp,
             {{"a", "1"}});
    case_map("map_zwsp_padded_alias_is_kept", "a", str("b"), zwsp + "x" + zwsp, {{"a", "1"}});
    case_map("map_alias_is_trimmed", "a", str("b"), "  x  ", {{"a", "1"}});
    case_map("map_inner_spaces_are_kept", "a", str("b"), "  x  y  ", {{"a", "1"}});
    case_map("map_absent_old_still_adds_alias", "x", str("b"), "y", {{"a", "1"}});
    case_map("map_absent_old_with_null_new_is_unchanged", "x", null_value(), "y", {{"a", "1"}});
    case_map("map_empty_input_adds_alias", "a", str("b"), "x", {});
    case_map("map_old_equals_new_blank_alias_removes", "a", str("a"), "",
             {{"a", "1"}, {"b", "2"}});
    case_map("map_newline_alias_is_escaped", "a", str("b"), "x\ny", {{"a", "1"}});
    case_map("map_unicode_ids", "\u043c\u043e\u0434\u0435\u043b\u044c",
             str("\u043d\u043e\u0432\u0430\u044f"),
             "\u043f\u0441\u0435\u0432\u0434\u043e\u043d\u0438\u043c",
             {{"\u043c\u043e\u0434\u0435\u043b\u044c", "1"}, {"b", "2"}});

    remove_stale(scenarios);
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, Format::Spaced);
    }
    std::cout << "wrote " << all.size() << " scenarios to " << scenarios.string() << "\n";
}

// sandbox-path-resolver/gen_scenarios.py — indent=2, numbered file names.
void profile_sandbox_path_resolver(const fs::path& scenarios) {
    std::vector<std::pair<std::string, json::Value>> entries;
    auto add = [&](const char* name, json::Value value) {
        entries.emplace_back(name, std::move(value));
    };
    auto resolve = [&](const char* name, const char* path, const char* home,
                       const char* shared) {
        add(name, object({{"op", str("resolve")},
                          {"path", str(path)},
                          {"home_mount_path", str(home)},
                          {"shared_storage_mount_path", str(shared)}}));
    };
    auto walk = [&](const char* name, std::vector<std::string> files, const char* physical,
                    const char* virtual_root, std::int64_t remaining) {
        std::vector<json::Value> items;
        for (auto& file : files) items.push_back(str(std::move(file)));
        add(name, object({{"op", str("walk")},
                          {"files", array(std::move(items))},
                          {"physical_root", str(physical)},
                          {"virtual_root", str(virtual_root)},
                          {"remaining", number(remaining)}}));
    };
    auto file_read = [&](const char* name, const char* path, std::int64_t offset,
                         std::int64_t limit, std::int64_t total_bytes, std::string content,
                         std::int64_t lines, std::int64_t total_lines, bool truncated) {
        add(name, object({{"op", str("file_read")},
                          {"path", str(path)},
                          {"offset", number(offset)},
                          {"limit", number(limit)},
                          {"total_bytes", number(total_bytes)},
                          {"content", str(std::move(content))},
                          {"lines", number(lines)},
                          {"total_lines", number(total_lines)},
                          {"truncated", boolean(truncated)}}));
    };

    const char* kHome = "/mnt/home";
    const char* kShared = "/mnt/shared";

    resolve("resolve-rootfs-simple", "foo.txt", kHome, kShared);
    resolve("resolve-rootfs-nested", "dir/bar.txt", kHome, kShared);
    resolve("resolve-rootfs-absolute", "/baz.txt", kHome, kShared);
    resolve("resolve-home-mount", "/mnt/home/foo.txt", kHome, kShared);
    resolve("resolve-home-nested", "/mnt/home/dir/bar.txt", kHome, kShared);
    resolve("resolve-shared-mount", "/mnt/shared/foo.txt", kHome, kShared);
    resolve("resolve-shared-nested", "/mnt/shared/dir/bar.txt", kHome, kShared);
    resolve("resolve-traversal-rootfs", "../../../etc/passwd", kHome, kShared);
    resolve("resolve-traversal-home", "/mnt/home/../../../etc/passwd", kHome, kShared);
    resolve("resolve-traversal-shared", "/mnt/shared/../../../etc/passwd", kHome, kShared);
    resolve("resolve-normalize-backslashes", "foo\\bar.txt", kHome, kShared);
    resolve("resolve-normalize-repeated-slashes", "/foo//bar///baz.txt", kHome, kShared);
    resolve("resolve-normalize-dot-segments", "foo/./bar/../baz.txt", kHome, kShared);

    const std::vector<std::string> walk_files = {"/a.txt",   "/b.md",        "/dir/c.txt",
                                                 "/dir/sub/d.txt", "/other/e.txt"};
    auto prefixed = [](const std::vector<std::string>& files, const std::string& prefix) {
        std::vector<std::string> result;
        for (const std::string& file : files) {
            std::string replaced;
            for (const char ch : file) {
                if (ch == '/') {
                    replaced += prefix;
                } else {
                    replaced.push_back(ch);
                }
            }
            result.push_back(std::move(replaced));
        }
        return result;
    };

    walk("walk-root-unlimited", walk_files, "/root", "/", -1);
    walk("walk-root-depth1", walk_files, "/root", "/", 1);
    walk("walk-root-depth2", walk_files, "/root", "/", 2);
    walk("walk-home-unlimited", prefixed(walk_files, "/mnt/home/"), "/root/home", "/mnt/home",
         -1);
    walk("walk-shared-unlimited", prefixed(walk_files, "/mnt/shared/"), "/root/shared",
         "/mnt/shared", -1);

    file_read("file-read-simple", "/test.txt", 0, 0, 11, "Hello World", 1, 1, false);
    file_read("file-read-offset", "/test.txt", 6, 0, 11, "World", 1, 1, false);
    file_read("file-read-limit", "/test.txt", 0, 5, 11, "Hello", 1, 1, false);
    file_read("file-read-truncated", "/large.txt", 0, 100, 10000, std::string(100, 'A'), 1,
              1000, true);

    remove_stale(scenarios);
    const FormatOptions pretty{Format::Pretty2, false};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "%03zu-", i + 1);
        write_scenario(scenarios, std::string(prefix) + entries[i].first, entries[i].second,
                       pretty);
    }
    std::cout << "wrote " << entries.size() << " scenarios to " << scenarios.string() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: agora_scenarios <profile> <stand_dir>\n"
                  << "profiles:\n"
                  << "  migration-20-35       migration-20-35/gen_scenarios.py\n"
                  << "  developer-mode-unlock developer-mode-unlock/gen_scenarios.py\n"
                  << "  shell-definitions     shell-definitions/gen_scenarios.py\n"
                  << "  cjk-text              cjk-text/gen_scenarios.py\n"
                  << "  embedding             embedding/gen_scenarios.py\n"
                  << "  native-backup-format  native-backup-format/gen_scenarios.py\n"
                  << "  malformed-tool-call   malformed-tool-call/gen_scenarios.py\n"
                  << "  provider-names        provider-names/gen_scenarios.py\n"
                  << "  shell-command-validation shell-command-validation/gen_scenarios.py\n"
                  << "  database-compatibility database-compatibility/gen_scenarios.py\n"
                  << "  context-budget        context-budget/gen_scenarios.py\n"
                  << "  memory-manager        memory-manager/gen_scenarios.py\n"
                  << "  provider-support      provider-support/gen_scenarios.py\n"
                  << "  run-branch-selection  run-branch-selection/gen_scenarios.py\n"
                  << "  custom-model-references custom-model-references/gen_scenarios.py\n"
                  << "  sandbox-path-resolver sandbox-path-resolver/gen_scenarios.py\n";
        return 2;
    }
    const std::string profile = argv[1];
    const fs::path stand = fs::path(argv[2]).lexically_normal();
    const fs::path scenarios = stand / "scenarios";

    if (profile == "migration-20-35") {
        profile_migration_20_35(scenarios);
    } else if (profile == "developer-mode-unlock") {
        profile_developer_mode_unlock(scenarios);
    } else if (profile == "shell-definitions") {
        profile_shell_definitions(scenarios);
    } else if (profile == "cjk-text") {
        profile_cjk_text(scenarios);
    } else if (profile == "embedding") {
        profile_embedding(scenarios);
    } else if (profile == "native-backup-format") {
        profile_native_backup_format(scenarios);
    } else if (profile == "malformed-tool-call") {
        profile_malformed_tool_call(scenarios);
    } else if (profile == "provider-names") {
        profile_provider_names(scenarios);
    } else if (profile == "shell-command-validation") {
        profile_shell_command_validation(scenarios);
    } else if (profile == "database-compatibility") {
        profile_database_compatibility(scenarios);
    } else if (profile == "context-budget") {
        profile_context_budget(scenarios);
    } else if (profile == "memory-manager") {
        profile_memory_manager(scenarios);
    } else if (profile == "provider-support") {
        profile_provider_support(scenarios);
    } else if (profile == "run-branch-selection") {
        profile_run_branch_selection(scenarios);
    } else if (profile == "custom-model-references") {
        profile_custom_model_references(scenarios);
    } else if (profile == "sandbox-path-resolver") {
        profile_sandbox_path_resolver(scenarios);
    } else {
        std::cerr << "unknown profile: " << profile << "\n";
        return 2;
    }
    return 0;
}