// tools/scenarios_main.cpp — C++ replacement for the golden-stand gen_scenarios.py scripts.
//
// Each profile reproduces one Python generator byte for byte. The committed scenarios/*.json
// corpora are the parity reference: running a profile must leave `git status` clean.
//
// Usage: agora_scenarios <profile> <stand_dir>
//   stand_dir is core/tools/golden/<stand>; scenarios are written to <stand>/scenarios.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <zlib.h>
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

// Python's float repr (json.dump uses it): shortest round-trip form, with ".0" kept on integral
// values and a lowercase "e+NN" exponent, exactly what std::to_chars(general) produces.
json::Value float_number(double value) {
    char buffer[64];
    const std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    std::string text(buffer, result.ptr);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
        text += ".0";
    }
    return literal(std::move(text));
}

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


// loop-policy/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_loop_policy(const fs::path& scenarios) {
    constexpr std::int64_t kLongMax = 9223372036854775807LL;
    constexpr std::int64_t kLongMin = (-9223372036854775807LL - 1);
    constexpr std::int64_t kIntMax = 2147483647LL;
    constexpr std::int64_t kIntMin = -2147483648LL;
    const std::string nbsp = "\u00a0";
    const std::string zwsp = "\u200b";
    std::vector<json::Value> all;
    auto add = [&](const char* name, const char* op,
                   std::vector<std::pair<std::string, json::Value>> extra) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)},
                                                                    {"op", str(op)}};
        for (auto& member : extra) members.push_back(std::move(member));
        all.push_back(object(std::move(members)));
    };
    auto validate = [&](const char* name, std::int64_t interval, std::int64_t cycles) {
        add(name, "validate",
            {{"intervalMs", number(interval)}, {"maxCycles", number(cycles)}});
    };
    auto normalize = [&](const char* name, json::Value prompt) {
        add(name, "normalizePrompt", {{"prompt", std::move(prompt)}});
    };
    auto execution = [&](const char* name, json::Value prompt) {
        add(name, "promptForExecution", {{"prompt", std::move(prompt)}});
    };
    auto fire = [&](const char* name, std::int64_t now, std::int64_t interval) {
        add(name, "nextFireAt", {{"now", number(now)}, {"intervalMs", number(interval)}});
    };
    auto revision = [&](const char* name, std::int64_t current) {
        add(name, "nextRevision", {{"current", number(current)}});
    };

    validate("validate_accepts_defaults", 60000, 10);
    validate("validate_accepts_min_interval", 10000, 10);
    validate("validate_accepts_max_interval", 604800000, 10);
    validate("validate_rejects_below_min_interval", 9999, 10);
    validate("validate_rejects_above_max_interval", 604800001, 10);
    validate("validate_rejects_zero_interval", 0, 10);
    validate("validate_rejects_negative_interval", -1, 10);
    validate("validate_rejects_long_min_interval", kLongMin, 10);
    validate("validate_rejects_long_max_interval", kLongMax, 10);

    validate("validate_accepts_min_cycles", 60000, 1);
    validate("validate_accepts_max_cycles", 60000, 100);
    validate("validate_rejects_zero_cycles", 60000, 0);
    validate("validate_rejects_101_cycles", 60000, 101);
    validate("validate_rejects_negative_cycles", 60000, -1);
    validate("validate_rejects_int_min_cycles", 60000, kIntMin);
    validate("validate_rejects_int_max_cycles", 60000, kIntMax);

    validate("validate_interval_error_wins", 0, 0);
    validate("validate_interval_error_wins_above_max", 604800001, 101);

    normalize("normalize_null", null_value());
    normalize("normalize_empty", str(""));
    normalize("normalize_spaces", str("   "));
    normalize("normalize_tabs_and_newlines", str("\t\n\r "));
    normalize("normalize_trims", str("  hello  "));
    normalize("normalize_keeps_inner_spaces", str(" a  b "));
    normalize("normalize_keeps_inner_newline", str("a\nb"));
    normalize("normalize_keeps_inner_tab_between_words", str(" a \t b "));
    normalize("normalize_unicode_text", str("  \u041f\u0440\u0438\u0432\u0435\u0442  "));
    normalize("normalize_nbsp_only_collapses", str(nbsp));
    normalize("normalize_nbsp_is_trimmed", str(nbsp + "x" + nbsp));
    normalize("normalize_zero_width_space_is_content", str(zwsp));

    execution("execution_null_falls_back", null_value());
    execution("execution_blank_falls_back", str("   "));
    execution("execution_nbsp_falls_back", str(nbsp));
    execution("execution_trims", str("  keep going  "));

    fire("fire_normal", 1000, 60000);
    fire("fire_exact_max", kLongMax - 60000, 60000);
    fire("fire_clamps_on_overflow", kLongMax - 60000 + 1, 60000);
    fire("fire_zero_interval", 123, 0);
    fire("fire_negative_now", -5, 10);
    fire("fire_max_now_zero_interval", kLongMax, 0);
    fire("fire_negative_interval_wraps_threshold", 0, -10);
    fire("fire_negative_interval_at_long_min_wraps_sum", kLongMin, -10);
    fire("fire_negative_interval_at_long_max", kLongMax, -10);

    revision("revision_from_zero", 0);
    revision("revision_from_forty_two", 42);
    revision("revision_from_max_minus_one", kLongMax - 1);
    revision("revision_at_max_wraps_to_zero", kLongMax);

    remove_stale(scenarios);
    std::vector<std::string> seen;
    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : all) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
            fail("duplicate scenario name: " + name);
        }
        seen.push_back(name);
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "Wrote " << all.size() << " scenarios\n";
}

// sandbox-virtual-paths/gen_scenarios.py — indent=2, numbered file names.
void profile_sandbox_virtual_paths(const fs::path& scenarios) {
    std::vector<std::pair<std::string, json::Value>> entries;
    auto add = [&](const char* name, json::Value value) {
        entries.emplace_back(name, std::move(value));
    };
    auto norm = [&](const char* name, const char* path) {
        add(name, object({{"op", str("normalize")}, {"path", str(path)}}));
    };
    auto glob = [&](const char* name, std::vector<std::string> files, const char* pattern) {
        std::vector<json::Value> items;
        for (auto& file : files) items.push_back(str(std::move(file)));
        add(name, object({{"op", str("glob")},
                          {"files", array(std::move(items))},
                          {"pattern", str(pattern)}}));
    };

    const std::vector<std::string> files = {"/a.txt",
                                            "/b.md",
                                            "/dir/a.txt",
                                            "/dir/b.md",
                                            "/dir/sub/c.txt",
                                            "/dir/sub/deep/d.md",
                                            "/other/e.txt"};

    norm("norm-empty", "");
    norm("norm-spaces", "   ");
    norm("norm-tabs", "\t\n");
    norm("norm-nbsp", "\u00a0");
    norm("norm-zero-width-space", "\u200b");
    norm("norm-relative", "foo");
    norm("norm-absolute", "/foo");
    norm("norm-relative-nested", "foo/bar");
    norm("norm-backslashes", "foo\\bar");
    norm("norm-unc", "\\\\server\\share");
    norm("norm-repeated-slashes", "/foo//bar///");
    norm("norm-root", "/");
    norm("norm-all-slashes", "///");
    norm("norm-trailing-slash", "/foo/");
    norm("norm-relative-trailing-slash", "foo/");
    norm("norm-padded", "  /foo/bar  ");
    norm("norm-dot-segment", "a/../b");
    norm("norm-dot", ".");
    norm("norm-dotdot", "..");
    norm("norm-tilde", "~");
    norm("norm-drive", "C:\\foo\\bar");
    norm("norm-unicode", "h\u00e9llo/\u4e16\u754c");
    norm("norm-internal-space", "a b");
    norm("norm-space-slash", " / ");
    norm("norm-double-leading-trailing", "//foo//");
    norm("norm-relative-double-trailing", "foo//");
    norm("norm-backslash-only", "\\");
    norm("norm-double-backslash", "a\\\\b");
    norm("norm-dot-segment-inner", "a/./b");
    norm("norm-two-slashes", "//");

    glob("glob-blank", files, "");
    glob("glob-space", files, " ");
    glob("glob-star-txt", files, "*.txt");
    glob("glob-doublestar-txt", files, "**/*.txt");
    glob("glob-dir-txt", files, "dir/*.txt");
    glob("glob-absolute-dir-txt", files, "/dir/*.txt");
    glob("glob-dir-doublestar-txt", files, "dir/**/*.txt");
    glob("glob-doublestar-md", files, "**/*.md");
    glob("glob-question-txt", files, "?.txt");
    glob("glob-a-star-txt", files, "a*.txt");
    glob("glob-class", files, "[ab].txt");
    glob("glob-negated-class", files, "[!a].txt");
    glob("glob-dir-class", files, "dir/[ab].txt");
    glob("glob-trailing-slash", files, "dir/");
    glob("glob-root", files, "/");
    glob("glob-doublestar", files, "**");
    glob("glob-star", files, "*");
    glob("glob-backslash-txt", files, "\\*.txt");
    glob("glob-dir-backslash-txt", files, "dir\\*.txt");
    glob("glob-no-match", files, "nope/*.txt");
    glob("glob-dir-sub-txt", files, "dir/sub/*.txt");
    glob("glob-dir-doublestar", files, "dir/**");
    glob("glob-deep-md", files, "**/deep/*.md");
    glob("glob-sub-doublestar", files, "**/sub/**");
    glob("glob-backslash-path", {"\\a\\b.txt"}, "a/b.txt");
    glob("glob-unicode", {"/h\u00e9llo/\u4e16\u754c.txt"}, "*.txt");
    glob("glob-empty-files", {}, "*.txt");
    glob("glob-padded-absolute", files, " /a.txt ");
    glob("glob-padded-name", files, "a.txt ");
    glob("glob-dir-star", files, "dir/*");
    glob("glob-exact", files, "/dir/sub/deep/d.md");
    glob("glob-exact-miss", files, "/dir/sub/deep/x.md");

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

// glob-matcher/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_glob_matcher(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto match = [&](const char* name, const char* pattern, const char* path) {
        all.push_back(object({{"name", str(name)},
                              {"mode", str("match")},
                              {"pattern", str(pattern)},
                              {"path", str(path)}}));
    };

    match("literal_exact", "a/b.txt", "a/b.txt");
    match("literal_mismatch", "a/b.txt", "a/c.txt");
    match("literal_prefix_only", "a", "ab");
    match("empty_both", "", "");
    match("empty_pattern_path", "", "a");
    match("pattern_empty_path", "a", "");
    match("literal_metacharacters", "a.b+c(d)", "a.b+c(d)");
    match("literal_dot_does_not_match_star", "a.b", "axb");

    match("star_suffix", "*.txt", "a.txt");
    match("star_suffix_empty_stem", "*.txt", ".txt");
    match("star_crosses_separator", "a*b", "a/b");
    match("star_middle", "a*c", "abc");
    match("star_newline_is_in_class", "a*b", "a\nb");
    match("star_repeated", "a*b*c", "abc");
    match("star_alone", "*", "anything");
    match("star_alone_empty", "*", "");

    match("globstar_leading_optional_dir", "**/foo", "foo");
    match("globstar_leading_deep", "**/foo", "a/b/foo");
    match("globstar_middle_collapse", "a/**/foo", "a/foo");
    match("globstar_middle_deep", "a/**/foo", "a/x/y/foo");
    match("globstar_whole", "**", "a/b/c");
    match("globstar_empty", "**", "");
    match("globstar_adjacent_stars", "a**b", "a/b");
    match("globstar_adjacent_newline_blocked", "a**b", "a\nb");
    match("globstar_trailing", "a/**", "a/");
    match("globstar_trailing_deep", "a/**", "a/b/c");
    match("globstar_prefix_globstar_suffix", "src/**/build", "src/main/build");
    match("globstar_star_mix", "**/*.kt", "a/b/c.kt");
    match("globstar_star_mix_no_dir", "**/*.kt", "c.kt");

    match("question_one_char", "a?c", "abc");
    match("question_requires_char", "a?c", "ac");
    match("question_excludes_separator", "a?c", "a/c");
    match("question_astral_is_one_code_point", "a?c", "a\U0001F600c");
    match("question_matches_newline", "a?c", "a\nc");
    match("question_alone", "?", "x");
    match("question_alone_separator", "?", "/");

    match("class_simple", "[abc]", "b");
    match("class_miss", "[abc]", "d");
    match("class_negated", "[!abc]", "d");
    match("class_negated_hit", "[!abc]", "a");
    match("class_empty_is_invalid_pattern", "[]", "]");
    match("class_bang_only_is_invalid_pattern", "[!]", "x");
    match("class_literal_open_bracket", "[[]", "[");
    match("class_literal_close_bracket", "[]a]", "]");
    match("class_escaped_backslash", "[\\\\]", "\\");
    match("class_caret_escaped_at_zero", "[^]", "^");
    match("class_caret_inside", "[a^]", "^");
    match("class_range", "[a-c]", "b");
    match("class_range_miss", "[a-c]", "d");
    match("class_with_separator_member", "[/a]", "/");
    match("class_unclosed_is_literal", "[abc", "[abc");
    match("class_in_middle", "a[0-9]z", "a5z");
    match("class_after_star", "*[0-9]", "abc7");
    match("class_bang_inside", "[a!b]", "!");
    match("class_multibyte", "[\u00e9\u00fc]", "\u00e9");
    match("class_trailing_dash", "[a-]", "-");
    match("class_leading_dash", "[-a]", "-");
    match("class_double_dash_literals", "[--a]", "b");
    match("class_double_dash_dash", "[--a]", "-");
    match("class_intersection", "[a&&b]", "a");
    match("class_intersection_empty_operand", "[a&&]", "a");
    match("class_intersection_leading", "[&&a]", "a");
    match("class_nested_open_is_invalid", "[a[b]", "a");
    match("class_negated_with_intersection", "[!a&&b]", "c");

    match("normalize_pattern_backslash", "src\\main", "src/main");
    match("normalize_path_backslash", "src/main", "src\\main");
    match("normalize_both_backslashes", "a\\b\\**\\c", "a/b/x/c");
    match("normalize_backslash_not_special", "\\*", "*");

    match("backtrack_multiple_stars", "a*a*a*a*b", "aa/a/a/b");
    match("backtrack_no_match", "a*a*a*a*b", "aa/a/a/c");
    match("backtrack_star_then_globstar", "*/**/x", "a/b/x");
    match("backtrack_empty_prefix", "*a", "a");
    match("deep_path", "**/node_modules/**/package.json",
          "a/node_modules/b/node_modules/package.json");

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

// skill-manager/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_skill_manager(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto add = [&](const char* name, const char* mode,
                   std::vector<std::pair<std::string, json::Value>> extra) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)},
                                                                    {"mode", str(mode)}};
        for (auto& member : extra) members.push_back(std::move(member));
        all.push_back(object(std::move(members)));
    };

    add("description_get_missing", "description_get", {{"file_name", str("missing.md")}});
    add("description_get_existing", "description_get",
        {{"file_name", str("guide.md")},
         {"setup_content", str("# Guide")},
         {"setup_description", str("Guide for agents")}});

    add("list_empty", "list", {});
    add("list_sorted", "list",
        {{"file_name", str("beta")},
         {"setup_content", str("beta")},
         {"setup_target_name", str("alpha.md")},
         {"setup_target_content", str("alpha")}});
    add("list_filters_non_md", "list",
        {{"file_name", str("keep.md")},
         {"setup_content", str("keep")},
         {"setup_raw_name", str("notes.txt")},
         {"setup_raw_content", str("not a skill")}});
    add("list_with_description", "list",
        {{"file_name", str("guide")},
         {"setup_content", str("# Guide")},
         {"setup_description", str("Guide for agents")}});

    add("catalog_empty", "catalog", {});
    add("catalog_with_files", "catalog",
        {{"file_name", str("guide")},
         {"setup_content", str("# Guide")},
         {"setup_description", str("Guide for agents")},
         {"setup_target_name", str("notes")},
         {"setup_target_content", str("scratch")}});
    add("catalog_description_newline", "catalog",
        {{"file_name", str("guide")},
         {"setup_content", str("# Guide")},
         {"setup_description", str("line one\n  line two  ")}});

    add("meta_empty", "meta", {});
    add("meta_save_restore", "meta_save",
        {{"meta_json", str("{\"guide.md\": \"Guide for agents\", \"notes.md\": \"\"}")}});
    add("meta_save_invalid", "meta_save", {{"meta_json", str("not json")}});
    add("meta_corrupt_list", "list", {{"setup_raw_meta", str("{")}});

    add("read_missing", "read", {{"file_name", str("missing.md")}});
    add("read_existing", "read",
        {{"file_name", str("guide.md")}, {"setup_content", str("Hello world")}});
    add("read_adds_md_extension", "read",
        {{"file_name", str("guide")}, {"setup_content", str("Hello world")}});

    add("create_basic", "create",
        {{"file_name", str("new.md")}, {"content_param", str("# Hello")}});
    add("create_adds_md_extension", "create",
        {{"file_name", str("notes")}, {"content_param", str("# Notes")}});
    add("create_with_description", "create",
        {{"file_name", str("guide.md")},
         {"content_param", str("Content")},
         {"description", str("Guide for agents")}});
    add("create_existing", "create",
        {{"file_name", str("existing.md")},
         {"setup_content", str("Original")},
         {"content_param", str("Duplicate")}});
    add("create_sanitizes_separators", "create",
        {{"file_name", str("nested/thing")}, {"content_param", str("# Nested")}});

    add("edit_replace", "edit",
        {{"file_name", str("guide.md")},
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
        {{"file_name", str("guide.md")},
         {"setup_content", str("Original")},
         {"description", str("New description")}});
    add("edit_description_unchanged", "edit",
        {{"file_name", str("guide.md")},
         {"setup_content", str("Original")},
         {"setup_description", str("Same description")},
         {"description", str("Same description")}});
    add("edit_patch", "edit",
        {{"file_name", str("guide.md")},
         {"setup_content", str("Hello old world")},
         {"old_string", str("old")},
         {"new_string", str("new")}});
    add("edit_patch_not_found", "edit",
        {{"file_name", str("guide.md")},
         {"setup_content", str("Hello world")},
         {"old_string", str("missing")},
         {"new_string", str("found")}});
    add("edit_patch_multiple", "edit",
        {{"file_name", str("guide.md")},
         {"setup_content", str("foo bar foo")},
         {"old_string", str("foo")},
         {"new_string", str("bar")}});
    add("edit_patch_no_match", "edit",
        {{"file_name", str("guide.md")},
         {"setup_content", str("Hello world")},
         {"old_string", str("")},
         {"new_string", str("new")}});
    add("edit_rename_conflict", "edit",
        {{"file_name", str("guide.md")},
         {"setup_content", str("Original")},
         {"new_name", str("existing.md")},
         {"setup_target_name", str("existing.md")},
         {"setup_target_content", str("Target")}});
    add("edit_no_changes", "edit",
        {{"file_name", str("guide.md")}, {"setup_content", str("Original")}});

    add("delete_existing", "delete",
        {{"file_name", str("guide.md")},
         {"setup_content", str("Content to delete")},
         {"setup_description", str("Guide for agents")}});
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


// provider-protocol-support/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_provider_protocol_support(const fs::path& scenarios) {
    std::vector<json::Value> cases;
    auto provider = [](const char* name, const char* protocol, const char* id,
                       std::vector<std::string> legacy, bool responses, bool cache,
                       const char* ttl) {
        std::vector<json::Value> names;
        for (auto& item : legacy) names.push_back(str(std::move(item)));
        return object({{"name", str(name)},
                       {"protocol", str(protocol)},
                       {"id", str(id)},
                       {"legacyNames", array(std::move(names))},
                       {"responsesApiEnabled", boolean(responses)},
                       {"anthropicCacheEnabled", boolean(cache)},
                       {"anthropicCacheTtl", str(ttl)}});
    };
    auto case_add = [&](const char* name, const char* provider_name,
                        std::vector<json::Value> providers,
                        std::vector<std::pair<std::string, json::Value>> fields) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"name", str(name)},
            {"providerName", str(provider_name)},
            {"customProviders", array(std::move(providers))},
        };
        for (auto& field : fields) members.push_back(std::move(field));
        cases.push_back(object(std::move(members)));
    };
    const char* kStable = "custom-provider-12345678-1234-4234-9234-123456789abc";

    case_add("builtin_openai_defaults", "OpenAI", {}, {});
    case_add("builtin_anthropic_defaults", "Anthropic", {}, {});
    case_add("builtin_openai_disabled", "OpenAI", {}, {{"builtInEnabled", boolean(false)}});
    case_add("builtin_anthropic_disabled", "Anthropic", {},
             {{"builtInAnthropicEnabled", boolean(false)}, {"builtInTtl", str("5m")}});
    case_add("builtin_anthropic_cache_disabled", "Anthropic", {},
             {{"builtInEnabled", boolean(false)}});
    case_add("builtin_other_no_custom", "Other", {}, {});

    case_add("custom_openai_by_name", "Relay",
             {provider("Relay", "openai", "", {}, false, true, "1h")}, {});
    case_add("custom_anthropic_by_name", "Relay",
             {provider("Relay", "anthropic", "", {}, false, true, "1h")}, {});
    case_add("custom_google_by_name", "Relay",
             {provider("Relay", "google", "", {}, false, true, "1h")}, {});
    case_add("custom_unknown_by_name", "Relay",
             {provider("Relay", "unknown", "", {}, false, true, "1h")}, {});
    case_add("custom_openai_by_stable_id", kStable,
             {provider("Renamed", "openai", kStable, {}, false, true, "1h")}, {});
    case_add("custom_anthropic_by_legacy", "Old Name",
             {provider("New Name", "anthropic", kStable, {"Old Name"}, false, true, "1h")}, {});

    case_add("custom_anthropic_cache_disabled", "Relay",
             {provider("Relay", "anthropic", "", {}, false, false, "5m")}, {});
    case_add("custom_anthropic_cache_enabled_ttl", "Relay",
             {provider("Relay", "anthropic", "", {}, false, true, "5m")}, {});
    case_add("custom_openai_cache_disabled", "Relay",
             {provider("Relay", "openai", "", {}, true, false, "1h")}, {});
    case_add("custom_openai_responses_enabled", "Relay",
             {provider("Relay", "openai", "", {}, true, true, "1h")}, {});
    case_add("custom_google_switches_ignored", "Relay",
             {provider("Relay", "google", "", {}, true, false, "1h")}, {});
    case_add("custom_unknown_cache_switch_still_reported", "Relay",
             {provider("Relay", "unknown", "", {}, false, false, "9m")}, {});

    case_add("first_match_is_anthropic_then_openai", "Relay",
             {provider("Relay", "anthropic", "", {}, false, false, "5m"),
              provider("Relay", "openai", "", {}, true, true, "1h")},
             {});
    case_add("first_match_is_google_then_anthropic", "Relay",
             {provider("Relay", "google", "", {}, false, false, "9m"),
              provider("Relay", "anthropic", "", {}, true, true, "1h")},
             {});
    case_add("first_match_is_legacy_then_name", "Old Name",
             {provider("First", "anthropic", kStable, {"Old Name"}, false, false, "1h"),
              provider("Old Name", "openai", "", {}, true, true, "1h")},
             {});
    case_add("any_openai_after_nonmatching", "Relay",
             {provider("Other", "anthropic", "", {}, false, true, "1h"),
              provider("Relay", "openai", "", {}, true, true, "1h")},
             {});
    case_add("any_anthropic_after_nonmatching", "Relay",
             {provider("Other", "openai", "", {}, false, true, "1h"),
              provider("Relay", "anthropic", "", {}, false, true, "1h")},
             {});

    case_add("builtin_openai_name_with_custom_google", "OpenAI",
             {provider("OpenAI", "google", "", {}, true, false, "9m")}, {});
    case_add("builtin_anthropic_name_with_custom_google", "Anthropic",
             {provider("Anthropic", "google", "", {}, false, false, "9m")},
             {{"builtInAnthropicEnabled", boolean(false)}, {"builtInTtl", str("30m")}});
    case_add("custom_name_case_sensitive", "relay",
             {provider("Relay", "openai", "", {}, false, true, "1h")}, {});
    case_add("custom_empty_id_uses_name", "Relay",
             {provider("Relay", "anthropic", "", {}, false, true, "1h")}, {});
    case_add("custom_legacy_names_order", "Alias B",
             {provider("Relay", "anthropic", kStable, {"Alias C", "Alias B"}, false, false,
                       "1h")},
             {});

    remove_stale(scenarios);
    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : cases) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "wrote " << cases.size() << " scenarios to " << scenarios.string() << "\n";
}

// new-chat-persist-dao/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_new_chat_persist_dao(const fs::path& scenarios) {
    auto persist = [](std::int64_t identifier, json::Value model_id, json::Value system_prompt_id,
                      json::Value settings, const char* draft_text, json::Value attachments) {
        return object({{"id", number(identifier)},
                       {"modelId", std::move(model_id)},
                       {"systemPromptId", std::move(system_prompt_id)},
                       {"conversationSettingsJson", std::move(settings)},
                       {"draftText", str(draft_text)},
                       {"draftAttachments", std::move(attachments)}});
    };
    auto op = [](const char* name,
                 std::vector<std::pair<std::string, json::Value>> arguments) {
        std::vector<std::pair<std::string, json::Value>> members = {{"op", str(name)}};
        for (auto& argument : arguments) members.push_back(std::move(argument));
        return object(std::move(members));
    };
    auto seed = [](std::vector<json::Value> rows) {
        return object({{"newChatPersist", array(std::move(rows))}});
    };
    auto delete_matching = [&](std::vector<std::pair<std::string, json::Value>> arguments) {
        return op("deleteNewChatPersistIfMatches", std::move(arguments));
    };
    auto upsert = [&](std::vector<std::pair<std::string, json::Value>> arguments) {
        return op("upsertNewChatPersist", std::move(arguments));
    };
    const FormatOptions pretty{Format::Pretty2, false};
    auto write = [&](const char* name, json::Value scenario) {
        write_scenario(scenarios, name, scenario, pretty);
    };

    write("01-empty-then-upsert",
          object({{"seed", seed({})},
                  {"steps", array({
                      op("observeNewChatPersist", {}),
                      op("getNewChatPersist", {}),
                      upsert({{"modelId", str("m1")},
                              {"systemPromptId", null_value()},
                              {"conversationSettingsJson", str("{\"t\":0.2}")},
                              {"draftText", str("hello")},
                              {"draftAttachments", str("[\"a.png\"]")}}),
                      op("observeNewChatPersist", {}),
                      op("getNewChatPersist", {}),
                      upsert({{"modelId", null_value()},
                              {"systemPromptId", str("p2")},
                              {"conversationSettingsJson", null_value()},
                              {"draftText", str("")},
                              {"draftAttachments", null_value()}}),
                      op("getNewChatPersist", {}),
                      op("observeNewChatPersist", {}),
                      delete_matching({{"modelId", null_value()},
                                       {"systemPromptId", str("p2")},
                                       {"conversationSettingsJson", null_value()},
                                       {"draftText", str("")},
                                       {"draftAttachments", null_value()}}),
                      op("observeNewChatPersist", {}),
                      op("getNewChatPersist", {}),
                      op("deleteNewChatPersist", {}),
                  })}}));

    write("02-seeded-row-matching",
          object({{"seed",
                   seed({persist(0, str("m1"), str("p1"), str("{\"x\":1}"), "draft",
                                 str("[\"a\"]"))})},
                  {"steps", array({
                      op("getNewChatPersist", {}),
                      op("observeNewChatPersist", {}),
                      delete_matching({{"modelId", str("m1")},
                                       {"systemPromptId", str("p1")},
                                       {"conversationSettingsJson", str("{\"x\":1}")},
                                       {"draftText", str("draft")},
                                       {"draftAttachments", str("[\"a\"]")}}),
                      op("getNewChatPersist", {}),
                      delete_matching({{"modelId", str("m1")},
                                       {"systemPromptId", str("p1")},
                                       {"conversationSettingsJson", str("{\"x\":1}")},
                                       {"draftText", str("draft")},
                                       {"draftAttachments", str("[\"a\"]")}}),
                  })}}));

    write("03-null-safe-matching",
          object({{"seed", seed({persist(0, null_value(), null_value(), null_value(), "x",
                                         null_value())})},
                  {"steps", array({
                      delete_matching({{"modelId", null_value()},
                                       {"systemPromptId", null_value()},
                                       {"conversationSettingsJson", null_value()},
                                       {"draftText", str("y")},
                                       {"draftAttachments", null_value()}}),
                      op("getNewChatPersist", {}),
                      delete_matching({{"modelId", null_value()},
                                       {"systemPromptId", null_value()},
                                       {"conversationSettingsJson", null_value()},
                                       {"draftText", str("x")},
                                       {"draftAttachments", null_value()}}),
                      op("getNewChatPersist", {}),
                      op("deleteNewChatPersist", {}),
                  })}}));

    write("04-invalid-entity",
          object({{"seed", seed({})},
                  {"steps", array({
                      upsert({{"id", number(1)}, {"draftText", str("bad")}}),
                      upsert({{"id", number(-1)}, {"draftText", str("worse")}}),
                      op("observeNewChatPersist", {}),
                      op("getNewChatPersist", {}),
                  })}}));

    write("05-upsert-clears-columns",
          object({{"seed",
                   seed({persist(0, str("m1"), str("p1"), str("{\"x\":1}"), "draft",
                                 str("[\"a\"]"))})},
                  {"steps", array({
                      upsert({{"modelId", null_value()},
                              {"systemPromptId", null_value()},
                              {"conversationSettingsJson", null_value()},
                              {"draftText", str("z")},
                              {"draftAttachments", null_value()}}),
                      op("getNewChatPersist", {}),
                      op("observeNewChatPersist", {}),
                      op("deleteNewChatPersist", {}),
                      op("observeNewChatPersist", {}),
                  })}}));
}

// model-thinking-resolver/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_model_thinking_resolver(const fs::path& scenarios) {
    std::vector<json::Value> cases;
    auto provider = [](const char* name, const char* protocol, const char* id,
                       std::vector<std::string> legacy) {
        std::vector<json::Value> names;
        for (auto& item : legacy) names.push_back(str(std::move(item)));
        return object({{"name", str(name)},
                       {"protocol", str(protocol)},
                       {"id", str(id)},
                       {"legacyNames", array(std::move(names))}});
    };
    auto case_add = [&](const char* name, const char* selected,
                        std::vector<json::Value> providers) {
        cases.push_back(object({{"name", str(name)},
                                {"selectedModel", str(selected)},
                                {"customProviders", array(std::move(providers))}}));
    };

    const std::vector<std::pair<const char*, const char*>> legacy = {
        {"legacy_gpt", "gpt-4o"},
        {"legacy_o1", "o1-preview"},
        {"legacy_o3", "o3-mini"},
        {"legacy_o4", "o4-mini"},
        {"legacy_o1_without_dash", "o100"},
        {"legacy_claude", "claude-3-opus"},
        {"legacy_deepseek", "deepseek-chat"},
        {"legacy_qwen", "qwen2.5-coder"},
        {"legacy_groq", "groq/llama-3"},
        {"legacy_google_models", "models/gemini-2.5-pro"},
        {"legacy_google_gemini", "gemini-1.5-flash"},
        {"legacy_unknown", "mistral-large"},
        {"legacy_uppercase_is_not_a_prefix", "GPT-4o"},
    };
    for (const auto& [name, model] : legacy) case_add(name, model, {});

    case_add("prefixed_openai", "OpenAI:gpt-4o", {});
    case_add("prefixed_empty_provider", ":gpt-4o", {});
    case_add("prefixed_empty_model", "OpenAI:", {});
    case_add("prefixed_first_colon_wins", "Provider:model:with:colons", {});
    case_add("prefixed_model_contains_colon", "Custom:org/model:tag", {});

    case_add("custom_name_match_anthropic", "Relay:claude-3-opus",
             {provider("Relay", "anthropic", "", {})});
    case_add("custom_name_match_google", "Relay:gemini-2.5-pro",
             {provider("Relay", "google", "", {})});
    case_add("custom_stable_id_match",
             "custom-provider-12345678-1234-4234-9234-123456789abc:gpt-4o",
             {provider("Renamed Relay", "anthropic",
                       "custom-provider-12345678-1234-4234-9234-123456789abc", {"Old Relay"})});
    case_add("custom_legacy_name_match", "Old Relay:gpt-4o",
             {provider("New Relay", "google",
                       "custom-provider-12345678-1234-4234-9234-123456789abc", {"Old Relay"})});
    case_add("custom_stable_id_case_insensitive_test",
             "CUSTOM-PROVIDER-ABCDEFAB-1234-4234-9234-123456789ABC:gpt-4o",
             {provider("Relay", "anthropic",
                       "CUSTOM-PROVIDER-ABCDEFAB-1234-4234-9234-123456789ABC", {})});
    case_add("custom_stable_id_with_spaces_is_still_stable",
             "  custom-provider-12345678-1234-4234-9234-123456789abc  :gpt-4o",
             {provider("Relay", "openai",
                       "  custom-provider-12345678-1234-4234-9234-123456789abc  ", {})});
    case_add("custom_first_match_wins", "Relay:gpt-4o",
             {provider("Relay", "anthropic", "", {}),
              provider("Relay", "google", "", {})});
    case_add("custom_name_is_case_sensitive", "relay:gpt-4o",
             {provider("Relay", "anthropic", "", {})});
    case_add("custom_name_does_not_match_builtin", "OpenAI:gpt-4o",
             {provider("Other", "anthropic", "", {})});
    case_add("custom_unknown_wire_protocol", "Relay:gpt-4o",
             {provider("Relay", "something-else", "", {})});
    case_add("custom_protocol_gemini_alias", "Relay:gpt-4o",
             {provider("Relay", "gemini", "", {})});
    case_add("custom_protocol_claude_alias", "Relay:gpt-4o",
             {provider("Relay", "claude", "", {})});
    case_add("custom_protocol_case_and_whitespace", "Relay:gpt-4o",
             {provider("Relay", "  AnThRoPiC  ", "", {})});
    case_add("custom_blank_protocol", "Relay:gpt-4o", {provider("Relay", "   ", "", {})});
    case_add("unknown_provider_name_can_match_custom", "Unknown:model",
             {provider("Unknown", "google", "", {})});
    case_add("custom_legacy_names_order", "Alias B:gpt-4o",
             {provider("Relay", "anthropic",
                       "custom-provider-12345678-1234-4234-9234-123456789abc",
                       {"Alias A", "Alias B", "Alias C"})});
    case_add("custom_empty_id_uses_name", "Relay:claude-3",
             {provider("Relay", "openai", "", {})});
    case_add("custom_provider_without_match", "Elsewhere:claude-3",
             {provider("Relay", "google", "", {})});

    remove_stale(scenarios);
    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : cases) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "wrote " << cases.size() << " scenarios to " << scenarios.string() << "\n";
}

// shell-tools/gen_scenarios.py — indent=2, ensure_ascii=False, maxLength derived from the
// compact JSON encoding of the full read payload (UTF-16 units, like Kotlin's String.length).
void profile_shell_tools(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto scenario = [&](const char* name, const char* mode,
                        std::vector<std::pair<std::string, json::Value>> extra) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)},
                                                                    {"mode", str(mode)}};
        for (auto& member : extra) members.push_back(std::move(member));
        all.push_back(object(std::move(members)));
    };

    scenario("args_mixed_values", "toolArgs",
             {{"arguments", str("{\"s\":\"x\",\"n\":1,\"b\":true,\"o\":{\"a\":1},"
                                "\"arr\":[1,2]}")},
              {"probeKey", str("s")}});
    scenario("args_blank_string_feeds_braces", "toolArgs",
             {{"arguments", str("")}, {"probeKey", str("k")}});
    scenario("args_whitespace_feeds_braces", "toolArgs",
             {{"arguments", str("   ")}, {"probeKey", str("k")}});
    scenario("args_malformed_returns_empty", "toolArgs",
             {{"arguments", str("{oops")}, {"probeKey", str("k")}});
    scenario("args_array_root_returns_empty", "toolArgs",
             {{"arguments", str("[1,2]")}, {"probeKey", str("k")}});
    scenario("args_primitive_root_returns_empty", "toolArgs",
             {{"arguments", str("42")}, {"probeKey", str("k")}});
    scenario("args_null_root_returns_empty", "toolArgs",
             {{"arguments", str("null")}, {"probeKey", str("k")}});
    scenario("args_duplicate_keys_last_wins_first_position", "toolArgs",
             {{"arguments", str("{\"a\":1,\"b\":2,\"a\":3}")}, {"probeKey", str("a")}});
    scenario("args_probe_null_value_is_literal", "toolArgs",
             {{"arguments", str("{\"z\":null}")}, {"probeKey", str("z")}});
    scenario("args_probe_object_value", "toolArgs",
             {{"arguments", str("{\"o\":{\"a\":1}}")}, {"probeKey", str("o")}});
    scenario("args_probe_array_value", "toolArgs",
             {{"arguments", str("{\"a\":[1,2]}")}, {"probeKey", str("a")}});
    scenario("args_probe_missing", "toolArgs",
             {{"arguments", str("{\"a\":1}")}, {"probeKey", str("nope")}});
    scenario("args_bool_true_literal", "toolArgs",
             {{"arguments", str("{\"b\":true}")}, {"probeKey", str("b")}});
    scenario("args_bool_false_literal", "toolArgs",
             {{"arguments", str("{\"b\":false}")}, {"probeKey", str("b")}});
    scenario("args_bool_quoted_true", "toolArgs",
             {{"arguments", str("{\"b\":\"true\"}")}, {"probeKey", str("b")}});
    scenario("args_bool_uppercase_false", "toolArgs",
             {{"arguments", str("{\"b\":\"TRUE\"}")}, {"probeKey", str("b")}});
    scenario("args_bool_number", "toolArgs",
             {{"arguments", str("{\"b\":1}")}, {"probeKey", str("b")}});

    scenario("error_full", "jsonError",
             {{"type", str("net")},
              {"message", str("boom")},
              {"server", str("srv")},
              {"command", str("ls")}});
    scenario("error_blank_type_omitted", "jsonError",
             {{"type", str("   ")}, {"message", str("boom")}});
    scenario("error_without_optional_fields", "jsonError",
             {{"type", str("net")}, {"message", str("boom")}});
    scenario("error_special_chars", "jsonError",
             {{"type", str("t")},
              {"message", str("quo\"te\nline\\slash")},
              {"server", null_value()},
              {"command", null_value()}});
    scenario("error_empty_message", "jsonError", {{"type", str("")}, {"message", str("")}});
    scenario("error_server_only", "jsonError",
             {{"type", str("t")}, {"message", str("m")}, {"server", str("box")}});

    auto full_json = [](const std::string& content) {
        const std::int64_t lines =
            content.empty() ? 0
                            : 1 + static_cast<std::int64_t>(
                                      std::count(content.begin(), content.end(), '\n'));
        const json::Value payload = object({{"type", str("file_read")},
                                            {"server", str("srv")},
                                            {"path", str("/tmp/f.txt")},
                                            {"content", str(content)},
                                            {"lines", number(lines)},
                                            {"total_lines", number(10)},
                                            {"total_bytes", number(100)},
                                            {"returned_bytes", number(
                                                 static_cast<std::int64_t>(content.size()))},
                                            {"offset", number(0)},
                                            {"limit", number(0)},
                                            {"truncated", boolean(false)}});
        return serialize(payload, FormatOptions{Format::Compact, false});
    };
    auto wrappers_len = [&](const std::string& content) {
        return static_cast<std::int64_t>(json::utf16_length(full_json(content))) -
               static_cast<std::int64_t>(json::utf16_length(content));
    };
    auto file_read = [&](const char* name, const std::string& content,
                         std::optional<std::int64_t> max_length, std::int64_t lines,
                         std::int64_t total_lines, std::int64_t total_bytes,
                         std::int64_t offset, std::int64_t limit, bool truncated) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"fileServer", str("srv")},
            {"filePath", str("/tmp/f.txt")},
            {"fileContent", str(content)},
            {"fileLines", number(lines)},
            {"totalLines", number(total_lines)},
            {"totalBytes", number(total_bytes)},
            {"offset", number(offset)},
            {"limit", number(limit)},
            {"fileTruncated", boolean(truncated)},
        };
        if (max_length) members.emplace_back("maxLength", number(*max_length));
        scenario(name, "fileRead", std::move(members));
    };

    file_read("file_small_fits_default_limit", "hello world", std::nullopt, 0, 10, 100, 0, 0,
              false);
    file_read("file_empty_content", "", 5000, 0, 10, 100, 0, 0, false);
    const std::string ascii_content(200, 'x');
    file_read("file_exact_fit_boundary", ascii_content,
              static_cast<std::int64_t>(full_json(ascii_content).size()), 0, 10, 100, 0, 0,
              false);
    file_read("file_truncated_ascii", ascii_content, 220, 0, 10, 100, 0, 0, false);
    file_read("file_tiny_limit_returns_over_limit_empty", ascii_content, 5, 0, 10, 100, 0, 0,
              false);
    file_read("file_limit_one", ascii_content, 1, 0, 10, 100, 0, 0, false);
    file_read("file_limit_zero_throws", ascii_content, 0, 0, 10, 100, 0, 0, false);
    file_read("file_limit_negative_throws", ascii_content, -7, 0, 10, 100, 0, 0, false);
    file_read("file_multiline_lines_counted", "one\ntwo\nthree", std::nullopt, 0, 10, 100, 0, 0,
              false);
    file_read("file_unicode_returned_bytes", "h\u00e9llo \u20ac", std::nullopt, 0, 10, 100, 0, 0,
              false);
    const std::string emoji_content = "AB\U0001F600CD\U0001F600EF";
    file_read("file_surrogate_pair_split_guard", emoji_content,
              wrappers_len(emoji_content) + 3, 0, 10, 100, 0, 0, false);
    file_read("file_surrogate_pair_exact_boundary", emoji_content,
              wrappers_len(emoji_content) + 4, 0, 10, 100, 0, 0, false);
    {
        std::string euro_content;
        for (int i = 0; i < 120; ++i) euro_content += "\u20ac";
        file_read("file_big_unicode_truncation", euro_content, 260, 0, 10, 100, 0, 0, false);
    }
    file_read("file_metrics_passthrough", "abc", std::nullopt, 0, 42, 999, 5, 77, true);

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


// attachment-clone-session/gen_scenarios.py — indent=2, ensure_ascii=False, rmtree.
void profile_attachment_clone_session(const fs::path& scenarios) {
    const json::Value meta = object({{"items", array({
        object({{"type", str("image")},
                {"file_name", str("a.png")},
                {"originalUri", str("file://platform_clone_workspace/sources/a.png")}}),
        object({{"type", str("file")},
                {"file_name", str("b.txt")},
                {"originalUri", str("https://example.com/b.txt")}}),
        object({{"type", str("file")}, {"file_name", str("c.txt")}}),
    })}});
    // json.dumps with the default separators, exactly what the Python generator embeds.
    const std::string meta_text = serialize(meta, FormatOptions{Format::Spaced, false});
    auto source = [](const char* name, const char* content) {
        return object({{"name", str(name)}, {"content", str(content)}});
    };
    const FormatOptions pretty{Format::Pretty2, false};
    auto write = [&](const char* name, json::Value scenario) {
        write_scenario(scenarios, name, scenario, pretty);
    };

    remove_stale(scenarios);
    write("01-clone-images-and-meta",
          object({{"sources", array({source("a.png", "png-bytes"),
                                     source("b.txt", "text-bytes")})},
                  {"messages", array({object({{"id", str("m1")},
                                              {"conversationId", str("c1")},
                                              {"text", str("with attachments")},
                                              {"images", array({str("platform_clone_workspace/"
                                                                    "sources/a.png")})},
                                              {"attachmentMeta", str(meta_text)}})})}}));

    write("02-clone-same-owner-shares-file",
          object({{"sources", array({source("a.png", "png-bytes")})},
                  {"messages", array({
                      object({{"id", str("m1")},
                              {"conversationId", str("c1")},
                              {"images", array({str("platform_clone_workspace/sources/a.png")})}}),
                      object({{"id", str("m2")},
                              {"conversationId", str("c1")},
                              {"ownerKey", str("owner-1")},
                              {"images", array({str("platform_clone_workspace/sources/a.png")})}}),
                      object({{"id", str("m3")},
                              {"conversationId", str("c1")},
                              {"ownerKey", str("owner-1")},
                              {"images", array({str("platform_clone_workspace/sources/a.png")})}}),
                  })}}));

    write("03-clone-different-owner-separates-file",
          object({{"sources", array({source("a.png", "png-bytes")})},
                  {"messages", array({
                      object({{"id", str("m1")},
                              {"conversationId", str("c1")},
                              {"ownerKey", str("owner-1")},
                              {"images", array({str("platform_clone_workspace/sources/a.png")})}}),
                      object({{"id", str("m2")},
                              {"conversationId", str("c1")},
                              {"ownerKey", str("owner-2")},
                              {"images", array({str("platform_clone_workspace/sources/a.png")})}}),
                  })}}));

    write("04-clone-missing-source-keeps-path",
          object({{"sources", array({})},
                  {"messages", array({object({{"id", str("m1")},
                                              {"conversationId", str("c1")},
                                              {"images", array({str("platform_clone_workspace/"
                                                                    "sources/missing.png")})}})})}}));

    write("05-clone-rollback-removes-files",
          object({{"sources", array({source("a.png", "png-bytes")})},
                  {"messages", array({object({{"id", str("m1")},
                                              {"conversationId", str("c1")},
                                              {"images", array({str("platform_clone_workspace/"
                                                                    "sources/a.png")})}})})},
                  {"commit", str("false")}}));

    write("06-clone-commit-keeps-files",
          object({{"sources", array({source("a.png", "png-bytes")})},
                  {"messages", array({object({{"id", str("m1")},
                                              {"conversationId", str("c1")},
                                              {"images", array({str("platform_clone_workspace/"
                                                                    "sources/a.png")})}})})},
                  {"commit", str("true")}}));

    write("07-clone-standalone-meta",
          object({{"sources", array({source("a.png", "png-bytes")})},
                  {"messages", array({})},
                  {"standaloneMeta", object({{"raw", str(meta_text)}})}}));

    write("08-clone-no-attachments",
          object({{"sources", array({})},
                  {"messages", array({object({{"id", str("m1")},
                                              {"conversationId", str("c1")},
                                              {"text", str("plain")},
                                              {"images", array({})}})})}}));
    std::cout << "Wrote 8 scenarios to " << scenarios.string() << "\n";
}

// duckduckgo/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_duckduckgo(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto parse_case = [&](const char* name, std::string html, std::int64_t max_results) {
        all.push_back(object({{"name", str(name)},
                              {"mode", str("parse")},
                              {"html", str(std::move(html))},
                              {"maxResults", number(max_results)}}));
    };
    auto search_case = [&](const char* name, std::vector<json::Value> pages,
                           std::int64_t max_results) {
        all.push_back(object({{"name", str(name)},
                              {"mode", str("search")},
                              {"maxResults", number(max_results)},
                              {"pages", array(std::move(pages))}}));
    };
    auto page_html = [](std::string html) { return object({{"html", str(std::move(html))}}); };
    auto row = [](const std::string& url, const std::string& title,
                  std::optional<std::string> snippet, const std::string& quote) {
        std::string anchor = "<a " + quote + "class=" + quote + "result-link" + quote + " href=" +
                             quote + url + quote + ">" + title + "</a>";
        if (snippet) {
            anchor += "<td " + quote + "class=" + quote + "result-snippet" + quote + ">" +
                      *snippet + "</td>";
        }
        return anchor;
    };
    auto page = [&](std::vector<std::string> rows, std::optional<std::string> vqd,
                    std::optional<std::string> offset, bool captcha,
                    const std::string& extra) {
        std::string html = "<html><table>";
        for (const std::string& item : rows) html += item;
        html += "</table>";
        if (vqd) html += "<form><input name=\"vqd\" value=\"" + *vqd + "\"></form>";
        if (offset) html += "<form><input name=\"s\" value=\"" + *offset + "\"></form>";
        if (captcha) html += "<div class=\"anomaly-modal\">challenge-form</div>";
        return html + extra;
    };
    auto redirect = [](const std::string& href) {
        return std::string("//duckduckgo.com/l/?") + href;
    };
    const std::string quote = "\"";

    parse_case("parse_single_result",
               page({row("https://example.com/a", "Alpha", "First snippet.", quote)},
                    std::nullopt, std::nullopt, false, ""),
               5);
    parse_case("parse_entities_sequential",
               page({row("http://x/",
                         "Fish &amp; Chips &lt;hot&gt; &quot;q&quot; &#x27;s&#39;&nbsp;x",
                         "A &amp;lt; B &amp;amp; C &gt; D", quote)},
                    std::nullopt, std::nullopt, false, ""),
               5);
    parse_case("parse_double_unescape",
               page({row("http://x/", "A &amp;lt; B", std::nullopt, quote)}, std::nullopt,
                    std::nullopt, false, ""),
               5);
    parse_case("parse_tags_stripped",
               page({row("http://x/", "<b>Bold</b> plain <i>it</i>", "<span>snip</span> tail",
                         quote)},
                    std::nullopt, std::nullopt, false, ""),
               5);
    parse_case("parse_nested_anchor_text",
               page({row("http://x/", "A<span>B</span>", std::nullopt, quote)}, std::nullopt,
                    std::nullopt, false, ""),
               5);
    parse_case("parse_mismatched_class_quotes",
               "<a class='result-link\" href=\"http://odd.example/\">Odd</a>", 5);
    parse_case("parse_missing_href", "<a class=\"result-link\">No href here</a>", 5);
    parse_case("parse_blank_title_filtered",
               "<a class=\"result-link\" href=\"http://x/\">&nbsp;   </a>", 5);
    parse_case("parse_max_results_cap",
               page({row("http://x/1", "One", std::nullopt, quote),
                     row("http://x/2", "Two", std::nullopt, quote),
                     row("http://x/3", "Three", std::nullopt, quote)},
                    std::nullopt, std::nullopt, false, ""),
               1);
    parse_case("parse_uddg_percent_encoded",
               page({row(redirect("uddg=https%3A%2F%2Fexample.com%2Fa%20b&rut=xyz"), "Wrapped",
                         std::nullopt, quote)},
                    std::nullopt, std::nullopt, false, ""),
               5);
    parse_case("parse_uddg_plus_decoded",
               page({row(redirect("uddg=https%3A%2F%2Fx.example%2Fa+b"), "Plus", std::nullopt,
                         quote)},
                    std::nullopt, std::nullopt, false, ""),
               5);
    parse_case("parse_protocol_relative",
               page({row("//example.org/path", "Proto", std::nullopt, quote)}, std::nullopt,
                    std::nullopt, false, ""),
               5);
    parse_case("parse_absolute_unchanged",
               page({row("https://keep.example/path?q=1", "Kept", std::nullopt, quote)},
                    std::nullopt, std::nullopt, false, ""),
               5);
    parse_case("parse_snippet_missing_pair",
               row("http://x/1", "One", "Snip one", quote) +
                   row("http://x/2", "Two", std::nullopt, quote),
               5);
    parse_case("parse_single_quotes_everywhere",
               "<a class='result-link' href='http://sq.example/'>Single</a>"
               "<td class='result-snippet'>Snip sq</td>",
               5);

    search_case("search_no_results",
                {page_html(page({"<p>nothing here</p>"}, std::nullopt, std::nullopt, false, ""))},
                5);
    search_case("search_first_page_no_vqd",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote)}, std::nullopt,
                                std::nullopt, false, ""))},
                5);
    search_case("search_success_single_page",
                {page_html(page({row("http://x/1", "One", "s1", quote),
                                 row("http://x/2", "Two", "s2", quote)},
                                "vqd-token", "10", false, ""))},
                5);
    search_case("search_captcha_first_page",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote)}, std::nullopt,
                                std::nullopt, true, ""))},
                5);
    search_case("search_captcha_partial",
                {page_html(page({row("http://x/1", "One", "s1", quote),
                                 row("http://x/2", "Two", "s2", quote)},
                                "tok", "10", false, "")),
                 page_html(page({row("http://x/9", "Nine", std::nullopt, quote)}, std::nullopt,
                                std::nullopt, true, ""))},
                5);
    search_case("search_network_first_missing_page", {}, 5);
    search_case("search_network_explicit",
                {object({{"error", str("DuckDuckGo returned HTTP 503.")}})}, 5);
    search_case("search_network_partial",
                {page_html(page({row("http://x/1", "One", "s1", quote)}, "tok", "10", false, "")),
                 object({{"error", str("Network error: timeout")}})},
                5);
    search_case("search_pagination_two_pages",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote),
                                 row("http://x/2", "Two", std::nullopt, quote)},
                                "tok", "10", false, "")),
                 page_html(page({row("http://x/3", "Three", std::nullopt, quote),
                                 row("http://x/4", "Four", std::nullopt, quote)},
                                "tok", "20", false, ""))},
                20);
    search_case("search_dedup_overlap_and_missing_offset",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote),
                                 row("http://x/2", "Two", std::nullopt, quote)},
                                "tok", "10", false, "")),
                 page_html(page({row("http://x/2", "Two again", std::nullopt, quote),
                                 row("http://x/3", "Three", std::nullopt, quote)},
                                std::nullopt, std::nullopt, false, ""))},
                20);
    search_case("search_no_new_results_breaks",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote),
                                 row("http://x/2", "Two", std::nullopt, quote)},
                                "tok", "10", false, "")),
                 page_html(page({row("http://x/1", "One", std::nullopt, quote),
                                 row("http://x/2", "Two", std::nullopt, quote)},
                                std::nullopt, std::nullopt, false, ""))},
                20);
    search_case("search_offset_malformed_breaks",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote)}, "tok",
                                "not-a-number", false, ""))},
                5);
    search_case("search_max_results_top_break",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote),
                                 row("http://x/2", "Two", std::nullopt, quote),
                                 row("http://x/3", "Three", std::nullopt, quote)},
                                "tok", "10", false, "")),
                 page_html(page({row("http://x/4", "Four", std::nullopt, quote)}, std::nullopt,
                                std::nullopt, false, ""))},
                1);
    search_case("search_vqd_only_from_first_page",
                {page_html(page({row("http://x/1", "One", std::nullopt, quote)}, "first-token",
                                "10", false, "")),
                 page_html(page({row("http://x/2", "Two", std::nullopt, quote)}, std::nullopt,
                                "20", false, "")),
                 page_html(page({row("http://x/3", "Three", std::nullopt, quote)}, std::nullopt,
                                "30", false, ""))},
                20);

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


// embedding-selector/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_embedding_selector(const fs::path& scenarios) {
    constexpr double kFloatMax = 3.4028235e38;
    std::vector<json::Value> all;
    auto add = [&](const char* name,
                   std::vector<std::pair<std::string, json::Value>> fields) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)}};
        for (auto& field : fields) members.push_back(std::move(field));
        all.push_back(object(std::move(members)));
    };
    auto floats = [](std::vector<double> values) {
        std::vector<json::Value> items;
        for (const double value : values) items.push_back(float_number(value));
        return array(std::move(items));
    };
    auto row = [&](std::int64_t id, std::vector<double> embedding, const char* message_id,
                   std::int64_t dimension) {
        return object({{"id", number(id)},
                       {"message_id", str(message_id)},
                       {"embedding", floats(std::move(embedding))},
                       {"dimension", number(dimension)}});
    };
    auto raw_page = [&](std::vector<json::Value> rows) { return array(std::move(rows)); };

    add("empty_query", {{"query", floats({})}, {"rows", array({row(1, {1.0, 0.0}, "m", -1)})}});
    add("non_positive_limit", {{"query", floats({1.0, 0.0})},
                               {"limit", number(0)},
                               {"rows", array({row(1, {1.0, 0.0}, "m", -1)})}});
    add("negative_limit", {{"query", floats({1.0, 0.0})},
                           {"limit", number(-3)},
                           {"rows", array({row(1, {1.0, 0.0}, "m", -1)})}});
    add("empty_rows", {{"query", floats({1.0, 0.0})}, {"rows", array({})}});
    add("non_positive_page_size_is_rejected",
        {{"page_size", number(0)},
         {"query", floats({1.0, 0.0})},
         {"rows", array({row(1, {1.0, 0.0}, "m", -1)})}});

    add("identical_vector", {{"query", floats({1.0, 0.0})},
                             {"threshold", float_number(0.5)},
                             {"limit", number(5)},
                             {"rows", array({row(1, {1.0, 0.0}, "exact", -1)})}});
    add("orthogonal_vector", {{"query", floats({1.0, 0.0})},
                              {"threshold", float_number(-1.0)},
                              {"limit", number(5)},
                              {"rows", array({row(1, {0.0, 1.0}, "perp", -1)})}});
    add("opposite_vector", {{"query", floats({1.0, 0.0})},
                            {"threshold", float_number(-2.0)},
                            {"limit", number(5)},
                            {"rows", array({row(1, {-1.0, 0.0}, "negated", -1)})}});
    add("three_dimensional", {{"query", floats({1.0, 1.0, 0.0})},
                              {"threshold", float_number(0.5)},
                              {"limit", number(5)},
                              {"rows", array({row(1, {1.0, 1.0, 0.0}, "same", -1),
                                              row(2, {1.0, 0.0, 0.0}, "half", -1)})}});
    add("scale_is_irrelevant", {{"query", floats({2.0, 0.0})},
                                {"threshold", float_number(0.99)},
                                {"limit", number(5)},
                                {"rows", array({row(1, {50.0, 0.0}, "scaled", -1)})}});

    add("threshold_excludes_row", {{"query", floats({1.0, 0.0})},
                                   {"threshold", float_number(0.9)},
                                   {"limit", number(5)},
                                   {"rows", array({row(1, {1.0, 1.0}, "below", -1)})}});
    add("threshold_is_exclusive", {{"query", floats({1.0, 0.0})},
                                   {"threshold", float_number(0.70710677)},
                                   {"limit", number(5)},
                                   {"rows", array({row(1, {1.0, 1.0}, "at", -1)})}});
    add("best_score_counts_sub_threshold_rows",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(0.99)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0, 1.0}, "below", -1), row(2, {1.0, 0.0}, "exact", -1)})}});

    add("dimension_mismatch_is_skipped",
        {{"query", floats({1.0, 0.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0, 0.0}, "short", 2),
                         row(2, {1.0, 0.0, 0.0}, "good", -1)})}});
    add("non_positive_dimension_is_skipped",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0, 0.0}, "zero", 0), row(2, {1.0, 0.0}, "good", -1)})}});
    add("negative_dimension_is_skipped",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0, 0.0}, "negative", -5),
                         row(2, {1.0, 0.0}, "good", -1)})}});
    add("byte_length_mismatch_is_skipped",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0}, "truncated", 2), row(2, {1.0, 0.0}, "good", -1)})}});
    add("all_rows_invalid",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0}, "a", 2), row(2, {1.0}, "b", 2)})}});

    add("overflowed_query_scores_zero",
        {{"query", floats({kFloatMax, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {1.0, 0.0}, "normal", -1)})}});
    add("overflowed_row_scores_zero",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {kFloatMax, 0.0}, "huge", -1)})}});
    add("overflowed_row_and_query_score_non_finite",
        {{"query", floats({kFloatMax, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"rows", array({row(1, {kFloatMax, 0.0}, "huge", -1),
                         row(2, {1.0, 0.0}, "normal", -1)})}});

    add("zero_row_scores_zero", {{"query", floats({1.0, 0.0})},
                                 {"threshold", float_number(-1.0)},
                                 {"limit", number(5)},
                                 {"rows", array({row(1, {0.0, 0.0}, "null-vector", -1)})}});
    add("zero_query_scores_zero", {{"query", floats({0.0, 0.0})},
                                   {"threshold", float_number(-1.0)},
                                   {"limit", number(5)},
                                   {"rows", array({row(1, {1.0, 0.0}, "any", -1)})}});

    add("limit_keeps_the_best_rows",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(2)},
         {"rows", array({row(1, {1.0, 0.0}, "best", -1),
                         row(2, {1.0, 1.0}, "mid", -1),
                         row(3, {-1.0, 0.0}, "worst", -1)})}});
    add("tie_break_prefers_the_lower_row_id",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(1)},
         {"rows", array({row(2, {1.0, 0.0}, "earlier", -1),
                         row(5, {1.0, 0.0}, "later", -1)})}});
    add("equal_scores_keep_the_lowest_ids",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(2)},
         {"rows", array({row(7, {1.0, 0.0}, "seven", -1),
                         row(8, {1.0, 0.0}, "eight", -1),
                         row(9, {1.0, 0.0}, "nine", -1)})}});
    add("candidates_are_ordered_by_score_then_id",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(10)},
         {"rows", array({row(1, {1.0, 0.0}, "one", -1),
                         row(2, {1.0, 1.0}, "half", -1),
                         row(3, {-1.0, 0.0}, "minus", -1),
                         row(4, {0.0, 1.0}, "zero", -1)})}});
    add("max_retained_tracks_the_high_water_mark",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(3)},
         {"rows", array({row(1, {1.0, 0.0}, "a", -1),
                         row(2, {1.0, 0.0}, "b", -1),
                         row(3, {1.0, 0.0}, "c", -1),
                         row(4, {1.0, 0.0}, "d", -1)})}});

    add("pages_are_followed_until_one_is_short",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(10)},
         {"page_size", number(2)},
         {"rows", array({row(1, {1.0, 0.0}, "a", -1),
                         row(2, {1.0, 0.0}, "b", -1),
                         row(3, {1.0, 0.0}, "c", -1)})}});
    add("full_final_page_still_terminates",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(10)},
         {"page_size", number(2)},
         {"rows", array({row(1, {1.0, 0.0}, "a", -1),
                         row(2, {1.0, 0.0}, "b", -1)})}});
    add("page_size_larger_than_the_catalog",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(10)},
         {"page_size", number(64)},
         {"rows", array({row(1, {1.0, 0.0}, "a", -1),
                         row(2, {1.0, 0.0}, "b", -1)})}});

    add("unordered_page_is_rejected",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"loader", str("raw")},
         {"raw_pages", array({raw_page({row(5, {1.0, 0.0}, "five", -1),
                                        row(3, {1.0, 0.0}, "three", -1)})})}});
    add("repeated_id_is_rejected",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"loader", str("raw")},
         {"raw_pages", array({raw_page({row(4, {1.0, 0.0}, "a", -1),
                                        row(4, {1.0, 0.0}, "b", -1)})})}});
    add("page_moving_backwards_is_rejected",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"page_size", number(1)},
         {"loader", str("raw")},
         {"raw_pages", array({raw_page({row(7, {1.0, 0.0}, "seven", -1)}),
                              raw_page({row(2, {1.0, 0.0}, "two", -1)})})}});
    add("oversized_page_is_rejected",
        {{"query", floats({1.0, 0.0})},
         {"threshold", float_number(-1.0)},
         {"limit", number(5)},
         {"page_size", number(2)},
         {"loader", str("oversized")},
         {"rows", array({row(1, {1.0, 0.0}, "a", -1),
                         row(2, {1.0, 0.0}, "b", -1),
                         row(3, {1.0, 0.0}, "c", -1)})}});

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

// migration-16-17/gen_scenarios.py — compact, ensure_ascii=False.
void profile_migration_16_17(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto conversation = [](const char* cid, const char* selections) {
        std::vector<std::pair<std::string, json::Value>> members = {{"id", str(cid)}};
        if (selections != nullptr) {
            members.emplace_back("selectedBranchesJson", str(selections));
        }
        return object(std::move(members));
    };
    auto message = [](const char* cid, const char* mid, const char* parent,
                      const char* participant, const char* status, std::int64_t timestamp) {
        return object({{"conversationId", str(cid)},
                       {"id", str(mid)},
                       {"parentId", parent == nullptr ? null_value() : str(parent)},
                       {"participant", str(participant)},
                       {"status", str(status)},
                       {"timestamp", number(timestamp)}});
    };
    auto user = [&](const char* cid, const char* mid, const char* parent, std::int64_t ts,
                    const char* status) {
        return message(cid, mid, parent, "USER", status, ts);
    };
    auto model = [&](const char* cid, const char* mid, const char* parent, std::int64_t ts,
                     const char* status) {
        return message(cid, mid, parent, "MODEL", status, ts);
    };
    auto case_add = [&](const char* name, std::vector<json::Value> conversations,
                        std::vector<json::Value> messages) {
        all.push_back(object({{"name", str(name)},
                              {"op", str("migrate")},
                              {"conversations", array(std::move(conversations))},
                              {"messages", array(std::move(messages))}}));
    };

    case_add("m16_empty", {}, {});
    case_add("m16_no_messages", {conversation("c1", nullptr)}, {});
    case_add("m16_single_user", {conversation("c1", nullptr)},
             {user("c1", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_user_model", {conversation("c1", nullptr)},
             {user("c1", "u", nullptr, 10, "SUCCESS"),
              model("c1", "m", "u", 11, "SUCCESS")});
    case_add("m16_two_boundaries", {conversation("c1", "{\"null\":\"u2\"}")},
             {user("c1", "u1", nullptr, 10, "SUCCESS"),
              model("c1", "m1", "u1", 11, "SUCCESS"),
              user("c1", "u2", "m1", 12, "SUCCESS"),
              model("c1", "m2", "u2", 13, "SUCCESS")});
    case_add("m16_two_conversations",
             {conversation("c1", "{\"null\":\"u1\"}"), conversation("c2", nullptr)},
             {user("c1", "u1", nullptr, 10, "SUCCESS"),
              model("c1", "m1", "u1", 11, "SUCCESS"),
              user("c2", "u2", nullptr, 20, "SUCCESS")});
    case_add("m16_orphan_model", {conversation("c1", nullptr)},
             {model("c1", "m", "gone", 10, "SUCCESS")});
    case_add("m16_selection_valid", {conversation("c1", "{\"m1\":\"u2\"}")},
             {user("c1", "u1", nullptr, 10, "SUCCESS"),
              model("c1", "m1", "u1", 11, "SUCCESS"),
              user("c1", "u2", "m1", 12, "SUCCESS"),
              model("c1", "m2", "u2", 13, "SUCCESS")});
    case_add("m16_selection_null_key", {conversation("c1", "{\"null\":\"u2\"}")},
             {user("c1", "u1", nullptr, 10, "SUCCESS"),
              user("c1", "u2", nullptr, 20, "SUCCESS")});
    case_add("m16_selection_null_key_first", {conversation("c1", "{\"null\":\"u1\"}")},
             {user("c1", "u1", nullptr, 10, "SUCCESS"),
              user("c1", "u2", nullptr, 20, "SUCCESS")});
    case_add("m16_selection_invalid_json", {conversation("c1", "not json")},
             {user("c1", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_selection_blank", {conversation("c1", "   ")},
             {user("c1", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_selection_non_string", {conversation("c1", "{\"a\":1}")},
             {user("c1", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_selection_non_string_known", {conversation("c1", "{\"null\":1}")},
             {user("c1", "1", nullptr, 10, "SUCCESS"),
              user("c1", "u1", nullptr, 20, "SUCCESS")});
    case_add("m16_selection_unknown_id", {conversation("c1", "{\"ghost\":\"u\"}")},
             {user("c1", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_unknown_enum_values", {conversation("c1", nullptr)},
             {message("c1", "u", nullptr, "USER", "SUCCESS", 10),
              message("c1", "m", "u", "BOGUS", "BOGUS", 11)});
    case_add("m16_unknown_status_only", {conversation("c1", nullptr)},
             {user("c1", "u", nullptr, 10, "SUCCESS"),
              message("c1", "m", "u", "MODEL", "BOGUS", 11)});
    case_add("m16_blank_conversation_id", {conversation("", nullptr)},
             {user("", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_duplicate_ids", {conversation("c1", nullptr)},
             {user("c1", "u", nullptr, 10, "SUCCESS"),
              model("c1", "u", "u", 11, "SUCCESS")});
    case_add("m16_unvisited_message", {},
             {user("c1", "u", nullptr, 10, "SUCCESS")});
    case_add("m16_conversation_order",
             {conversation("c2", "{\"null\":\"u2\"}"), conversation("c1", "{\"null\":\"u1\"}")},
             {user("c2", "u2", nullptr, 20, "SUCCESS"),
              user("c1", "u1", nullptr, 10, "SUCCESS")});

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


// chat-provider-context-dao/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_chat_provider_context_dao(const fs::path& scenarios) {
    auto conversation = [](const char* identifier, const char* selected_branches) {
        return object({{"id", str(identifier)},
                       {"selectedBranchesJson",
                        selected_branches == nullptr ? null_value() : str(selected_branches)}});
    };
    auto run = [](const std::string& conversation_id) {
        return object({{"id", str("run-" + conversation_id)},
                       {"conversationId", str(conversation_id)},
                       {"status", str("COMPLETED")},
                       {"startedAt", number(100)},
                       {"lastCheckpointAt", number(100)},
                       {"endedAt", number(200)},
                       {"endReason", str("MODEL_COMPLETED")},
                       {"currentPass", number(0)},
                       {"legacyAmbiguous", boolean(false)}});
    };
    struct MessageOptions {
        std::int64_t timestamp = 0;
        const char* status = "SUCCESS";
        const char* participant = "MODEL";
        std::int64_t token_count = 0;
        const char* parent_id = nullptr;
        const char* model_name = nullptr;
        std::int64_t run_sequence = -1;
        std::optional<std::int64_t> consumed_at_pass;
    };
    auto message = [](const char* identifier, const std::string& conversation_id,
                      const MessageOptions& options) {
        std::vector<std::pair<std::string, json::Value>> row = {
            {"id", str(identifier)},
            {"conversationId", str(conversation_id)},
            {"status", str(options.status)},
            {"participant", str(options.participant)},
            {"timestamp", number(options.timestamp)},
            {"tokenCount", number(options.token_count)},
            {"runId", str("run-" + conversation_id)},
            {"runSequence", number(options.run_sequence)},
        };
        if (options.parent_id != nullptr) row.emplace_back("parentId", str(options.parent_id));
        if (options.model_name != nullptr) row.emplace_back("modelName", str(options.model_name));
        if (options.consumed_at_pass) {
            row.emplace_back("consumedAtPass", number(*options.consumed_at_pass));
        }
        return object(std::move(row));
    };
    auto seed = [&](std::vector<json::Value> conversations, std::vector<json::Value> messages) {
        std::vector<json::Value> runs;
        for (const json::Value& row : conversations) {
            std::string id;
            for (const json::Member& member : row.members) {
                if (member.key == "id") id = member.value.text;
            }
            runs.push_back(run(id));
        }
        return object({{"conversations", array(std::move(conversations))},
                       {"runs", array(std::move(runs))},
                       {"messages", array(std::move(messages))}});
    };
    auto op = [](const char* name, const char* conversation_id) {
        return object({{"op", str(name)}, {"conversationId", str(conversation_id)}});
    };
    const FormatOptions pretty{Format::Pretty2, false};
    auto write = [&](const char* name, json::Value scenario) {
        write_scenario(scenarios, name, scenario, pretty);
    };

    write("01-provider-context-state",
          object({{"seed",
                   seed({conversation("c-1", "{\"m-1\":\"m-2\"}"), conversation("c-2", nullptr),
                         conversation("c-3", ""), conversation("c-4", "[]")},
                        {})},
                  {"steps", array({op("getProviderContextState", "c-1"),
                                   op("getProviderContextState", "c-2"),
                                   op("getProviderContextState", "c-3"),
                                   op("getProviderContextState", "c-4"),
                                   op("getProviderContextState", "missing")})}}));

    write("02-message-topology",
          object({{"seed",
                   seed({conversation("c-1", nullptr), conversation("c-2", nullptr)},
                        {message("m-02", "c-1",
                                 MessageOptions{10, "THINKING", "USER", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-03", "c-1",
                                 MessageOptions{10, "ERROR", "ERROR", 7, "m-02", "o3", 0, 0}),
                         message("m-04", "c-1",
                                 MessageOptions{20, "STOPPED", "MODEL", 3, "m-03", "gpt-5", 1,
                                                1}),
                         message("m-10", "c-1",
                                 MessageOptions{30, "SUCCESS", "MODEL", 5, "m-04", "gpt-5", 2,
                                                2}),
                         message("m-05", "c-1",
                                 MessageOptions{40, "SENDING", "USER", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-06", "c-1",
                                 MessageOptions{50, "TRANSCRIBING", "USER", 0, nullptr, nullptr,
                                                -1, std::nullopt}),
                         message("m-07", "c-1",
                                 MessageOptions{60, "TOOL_CALLING", "MODEL", 0, nullptr, nullptr,
                                                -1, std::nullopt}),
                         message("m-01", "c-2",
                                 MessageOptions{5, "SUCCESS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt})})},
                  {"steps", array({op("getMessageContextTopology", "c-1"),
                                   op("getMessageContextTopology", "c-2"),
                                   op("getMessageContextTopology", "missing"),
                                   op("getMessageContextTopology", "")})}}));

    write("03-observe-topology",
          object({{"seed",
                   seed({conversation("c-1", nullptr), conversation("c-2", nullptr)},
                        {message("m-02", "c-1",
                                 MessageOptions{1, "STOPPED", "USER", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-01", "c-1",
                                 MessageOptions{2, "SUCCESS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-03", "c-2",
                                 MessageOptions{0, "SUCCESS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt})})},
                  {"steps", array({op("observeMessageContextTopology", "c-1"),
                                   op("observeMessageContextTopology", "c-2"),
                                   op("observeMessageContextTopology", "missing")})}}));

    write("04-topology-snapshot",
          object({{"seed",
                   seed({conversation("c-01", "{\"m-1\":\"m-2\"}"),
                         conversation("c-02", nullptr), conversation("c-03", ""),
                         conversation("c-04", "[]")},
                        {message("m-02", "c-01",
                                 MessageOptions{20, "SUCCESS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-01", "c-01",
                                 MessageOptions{10, "THINKING", "USER", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-03", "c-02",
                                 MessageOptions{5, "SUCCESS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-04", "c-03",
                                 MessageOptions{6, "SUCCESS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt})})},
                  {"steps", array({op("getProviderContextTopologySnapshot", "c-01"),
                                   op("getProviderContextTopologySnapshot", "c-02"),
                                   op("getProviderContextTopologySnapshot", "c-03"),
                                   op("getProviderContextTopologySnapshot", "c-04"),
                                   op("getProviderContextTopologySnapshot", "missing")})}}));

    write("05-invalid-enums",
          object({{"seed",
                   seed({conversation("c-1", nullptr), conversation("c-2", nullptr)},
                        {message("m-01", "c-1",
                                 MessageOptions{0, "SUCCESS", "BOGUS", 0, nullptr, nullptr, -1,
                                                std::nullopt}),
                         message("m-02", "c-2",
                                 MessageOptions{0, "BOGUS", "MODEL", 0, nullptr, nullptr, -1,
                                                std::nullopt})})},
                  {"steps", array({op("getMessageContextTopology", "c-1"),
                                   op("getMessageContextTopology", "c-2"),
                                   op("getProviderContextTopologySnapshot", "c-1")})}}));
}

// openai-service-tiers/gen_scenarios.py — compact, ensure_ascii=False.
void profile_openai_service_tiers(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto case_add = [&](const char* name,
                        std::vector<std::pair<std::string, json::Value>> fields) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)}};
        for (auto& field : fields) members.push_back(std::move(field));
        all.push_back(object(std::move(members)));
    };
    auto normalize = [&](const char* name, json::Value value) {
        case_add(name, {{"op", str("normalize")}, {"value", std::move(value)}});
    };
    auto available = [&](const char* name, json::Value model, bool official) {
        case_add(name, {{"op", str("available")},
                        {"model", std::move(model)},
                        {"official", boolean(official)}});
    };
    auto mapped = [&](const char* name, json::Value value, json::Value model, bool official) {
        case_add(name, {{"op", str("mapped")},
                        {"value", std::move(value)},
                        {"model", std::move(model)},
                        {"official", boolean(official)}});
    };
    auto index = [&](const char* name, json::Value value) {
        case_add(name, {{"op", str("index")}, {"value", std::move(value)}});
    };
    auto tier = [&](const char* name, std::int64_t index_value) {
        case_add(name, {{"op", str("tier")}, {"index", number(index_value)}});
    };
    auto request = [&](const char* name, bool enabled, json::Value value, bool responses,
                       json::Value model, bool official) {
        case_add(name, {{"op", str("request")},
                        {"enabled", boolean(enabled)},
                        {"value", std::move(value)},
                        {"responses", boolean(responses)},
                        {"model", std::move(model)},
                        {"official", boolean(official)}});
    };

    normalize("normalize_null", null_value());
    normalize("normalize_empty", str(""));
    normalize("normalize_blank", str("   "));
    normalize("normalize_auto", str("auto"));
    normalize("normalize_default", str("default"));
    normalize("normalize_flex", str("flex"));
    normalize("normalize_fast", str("fast"));
    normalize("normalize_ultrafast", str("ultrafast"));
    normalize("normalize_uppercase", str("DEFAULT"));
    normalize("normalize_padded", str(" Flex "));
    normalize("normalize_tabs", str("\tFAST\n"));
    normalize("normalize_nbsp_padded", str("\u00a0flex\u00a0"));
    normalize("normalize_scale_alias", str("scale"));
    normalize("normalize_scale_alias_upper", str(" SCALE "));
    normalize("normalize_priority_alias", str("priority"));
    normalize("normalize_priority_alias_mixed", str("PRIORITY"));
    normalize("normalize_unknown", str("bogus"));
    normalize("normalize_partial", str("flexx"));

    available("available_all_null", null_value(), true);
    available("available_empty_model", str(""), true);
    available("available_gpt54", str("gpt-5.4"), true);
    available("available_gpt54mini", str("gpt-5.4-mini"), true);
    available("available_gpt41mini", str("gpt-4.1-mini"), true);
    available("available_o3pro", str("o3-pro"), true);
    available("available_gpt55pro", str("gpt-5.5-pro"), true);
    available("available_gpt4o", str("gpt-4o"), true);
    available("available_gpt52pro", str("gpt-5.2-pro"), true);
    available("available_gpt5pro", str("gpt-5-pro"), true);
    available("available_gpt56sol", str("gpt-5.6-sol"), true);
    available("available_gpt56luna", str("gpt-5.6-luna"), true);
    available("available_codex", str("gpt-5.3-codex"), true);
    available("available_o3", str("o3"), true);
    available("available_o4mini", str("o4-mini"), true);
    available("available_gpt41", str("gpt-4.1"), true);
    available("available_unknown", str("gpt-x"), true);
    available("available_case_padded", str(" GPT-5-PRO "), true);
    available("available_gpt6astra", str("GPT-6-ASTRA"), true);
    available("available_o1pro", str("o1-pro"), true);
    available("available_davinci", str("davinci-002"), true);
    available("available_custom_provider", str("gpt-5-pro"), false);
    available("available_custom_provider_unknown", str("whatever"), false);

    mapped("mapped_null", null_value(), str("gpt-5-pro"), true);
    mapped("mapped_auto", str("auto"), str("gpt-5-pro"), true);
    mapped("mapped_default", str("default"), str("gpt-5-pro"), true);
    mapped("mapped_flex_supported", str("flex"), str("gpt-5.5"), true);
    mapped("mapped_flex_unsupported", str("flex"), str("gpt-4o"), true);
    mapped("mapped_fast_supported", str("fast"), str("gpt-4o"), true);
    mapped("mapped_fast_unsupported", str("fast"), str("gpt-5-pro"), true);
    mapped("mapped_ultrafast_supported", str("ultrafast"), str("gpt-5.6-sol"), true);
    mapped("mapped_ultrafast_to_fast_luna", str("ultrafast"), str("gpt-5.6-luna"), true);
    mapped("mapped_ultrafast_to_fast_54", str("ultrafast"), str("gpt-5.4"), true);
    mapped("mapped_ultrafast_unknown_model", str("ultrafast"), str("unknown-model"), true);
    mapped("mapped_fast_on_sol", str("fast"), str("gpt-5.6-sol"), true);
    mapped("mapped_priority_gpt5pro", str("PRIORITY"), str("gpt-5-pro"), true);
    mapped("mapped_priority_gpt4o", str("PRIORITY"), str("gpt-4o"), true);
    mapped("mapped_scale", str("scale"), str("gpt-5-pro"), true);
    mapped("mapped_flex_custom_provider", str("flex"), str("gpt-5-pro"), false);
    mapped("mapped_ultrafast_custom_provider", str("ultrafast"), str("gpt-5-pro"), false);
    mapped("mapped_flex_gpt56sol", str("flex"), str("gpt-5.6-sol"), true);
    mapped("mapped_fast_standard_model", str("fast"), str("gpt-3.5-turbo"), true);
    mapped("mapped_padded_value", str(" Fast "), str("gpt-5-pro"), true);
    mapped("mapped_null_model", str("flex"), null_value(), true);

    index("index_null", null_value());
    index("index_empty", str(""));
    index("index_auto", str("auto"));
    index("index_default", str("default"));
    index("index_flex", str("flex"));
    index("index_fast", str("fast"));
    index("index_ultrafast", str("ultrafast"));
    index("index_priority", str("PRIORITY"));
    index("index_scale", str("SCALE"));
    index("index_unknown", str("nope"));
    index("index_padded", str(" FLEX "));
    index("index_uppercase", str("FLEX"));

    tier("tier_negative", -5);
    tier("tier_zero", 0);
    tier("tier_one", 1);
    tier("tier_two", 2);
    tier("tier_three", 3);
    tier("tier_four", 4);
    tier("tier_large", 99);

    request("request_disabled", false, str("flex"), true, str("gpt-5.5"), true);
    request("request_responses_off", true, str("flex"), false, str("gpt-5.5"), true);
    request("request_enabled_flex", true, str("flex"), true, str("gpt-5.5"), true);
    request("request_mapped_ultrafast", true, str("ultrafast"), true, str("gpt-5.4"), true);
    request("request_null_value", true, null_value(), true, str("gpt-5.4"), true);
    request("request_both_off", false, str("ultrafast"), false, str("gpt-5.6-sol"), true);
    request("request_flex_standard_model", true, str("flex"), true, str("gpt-5-pro"), true);
    request("request_custom_provider", true, str("flex"), true, str("gpt-5-pro"), false);
    request("request_priority_gpt4o", true, str("PRIORITY"), true, str("gpt-4o"), true);
    request("request_disabled_but_null_model", false, str("fast"), true, null_value(), true);
    request("request_enabled_ultrafast_sol", true, str("ultrafast"), true, str("gpt-5.6-sol"),
            true);

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


// conch-sse/gen_scenarios.py — indent=2, ensure_ascii=False; frame data uses json.dumps defaults.
void profile_conch_sse(const fs::path& scenarios) {
    constexpr std::int64_t kBudget = 1 << 20;
    std::vector<json::Value> all;
    auto add = [&](const char* name, std::vector<std::string> lines, bool encrypted,
                   const char* decrypt_error) {
        std::vector<json::Value> items;
        for (auto& line : lines) items.push_back(str(std::move(line)));
        std::vector<std::pair<std::string, json::Value>> members = {
            {"name", str(name)},
            {"encrypted", boolean(encrypted)},
            {"lines", array(std::move(items))},
        };
        if (decrypt_error != nullptr) members.emplace_back("decrypt_error", str(decrypt_error));
        all.push_back(object(std::move(members)));
    };
    auto frame_data = [](std::vector<std::pair<std::string, json::Value>> members) {
        return object(std::move(members));
    };
    auto frames = [&](bool encrypted,
                      std::vector<std::pair<std::string, json::Value>> pairs) {
        std::vector<std::string> out;
        std::int64_t sequence = 0;
        for (auto& [event, data] : pairs) {
            json::Value payload = data;
            if (encrypted) {
                payload.members.push_back(json::Member{"_conch_event", str(event)});
                payload.members.push_back(
                    json::Member{"_conch_sequence", number(sequence)});
                sequence += 1;
            }
            out.push_back("event: " + event);
            out.push_back("data: " + serialize(payload, FormatOptions{Format::Spaced, false}));
        }
        return out;
    };
    auto long_line = [&](const std::string& text) {
        return frames(false, {{"line", frame_data({{"line", str(text)},
                                                    {"stream", str("stdout")}})},
                              {"line", frame_data({{"line", str("tail")},
                                                    {"stream", str("stdout")}})},
                              {"result", frame_data({{"exit_code", number(0)}})}});
    };

    add("single_line_then_result",
        frames(false, {{"line", frame_data({{"line", str("hello")},
                                             {"stream", str("stdout")}})},
                       {"result", frame_data({{"exit_code", number(0)}})}}),
        false, nullptr);
    add("stdout_and_stderr_interleave",
        frames(false, {{"line", frame_data({{"line", str("out")},
                                             {"stream", str("stdout")}})},
                       {"line", frame_data({{"line", str("err")},
                                             {"stream", str("stderr")}})},
                       {"result", frame_data({{"exit_code", number(3)}})}}),
        false, nullptr);
    add("blank_line_between_frames_is_ignored",
        {"event: line", "data: {\"line\": \"hi\", \"stream\": \"stdout\"}", "", "event: result",
         "data: {\"exit_code\": 0}"},
        false, nullptr);
    add("empty_line_text_is_allowed",
        frames(false, {{"line", frame_data({{"line", str("")},
                                             {"stream", str("stdout")}})},
                       {"result", frame_data({{"exit_code", number(0)}})}}),
        false, nullptr);
    add("embedded_newlines_are_preserved",
        frames(false, {{"line", frame_data({{"line", str("one\ntwo")},
                                             {"stream", str("stdout")}})},
                       {"result", frame_data({{"exit_code", number(0)}})}}),
        false, nullptr);
    add("unknown_fields_are_ignored",
        {"event: line", "data: {\"line\": \"hi\", \"stream\": \"stdout\", \"future_key\": [1, 2]}",
         "event: result", "data: {\"exit_code\": 0, \"elapsed_ms\": 12}"},
        false, nullptr);
    add("warning_is_recorded",
        frames(false, {{"warning", frame_data({{"message", str("careful")}})},
                       {"result", frame_data({{"exit_code", number(0)}})}}),
        false, nullptr);
    add("multiple_warnings_are_joined",
        frames(false, {{"warning", frame_data({{"message", str("first")}})},
                       {"warning", frame_data({{"message", str("second")}})},
                       {"result", frame_data({{"exit_code", number(0)}})}}),
        false, nullptr);
    add("error_event_times_out",
        frames(false, {{"error", frame_data({{"message", str("killed")},
                                              {"timed_out", boolean(true)}})}}),
        false, nullptr);
    add("error_event_does_not_time_out",
        frames(false, {{"error", frame_data({{"message", str("refused")},
                                              {"timed_out", boolean(false)}})}}),
        false, nullptr);
    add("error_event_without_timeout_field",
        frames(false, {{"error", frame_data({{"message", str("refused")}})}}), false, nullptr);
    add("no_terminal_frame_keeps_the_streamed_output",
        frames(false, {{"line", frame_data({{"line", str("partial")},
                                             {"stream", str("stdout")}})}}),
        false, nullptr);

    add("encrypted_handshake_and_sequence",
        frames(true, {{"line", frame_data({{"line", str("secure")},
                                            {"stream", str("stdout")}})},
                      {"result", frame_data({{"exit_code", number(0)}})}}),
        true, nullptr);
    add("encrypted_sequence_mismatch",
        {"event: line",
         "data: {\"line\": \"x\", \"stream\": \"stdout\", \"_conch_event\": \"line\", "
         "\"_conch_sequence\": 1}"},
        true, nullptr);
    add("encrypted_sequence_repeated",
        {"event: line",
         "data: {\"line\": \"x\", \"stream\": \"stdout\", \"_conch_event\": \"line\", "
         "\"_conch_sequence\": 0}",
         "event: result",
         "data: {\"exit_code\": 0, \"_conch_event\": \"result\", \"_conch_sequence\": 0}"},
        true, nullptr);
    add("encrypted_sequence_is_a_string",
        {"event: line",
         "data: {\"line\": \"x\", \"stream\": \"stdout\", \"_conch_event\": \"line\", "
         "\"_conch_sequence\": \"0\"}"},
        true, nullptr);
    add("encrypted_missing_event_field",
        {"event: line",
         "data: {\"line\": \"x\", \"stream\": \"stdout\", \"_conch_sequence\": 0}"},
        true, nullptr);
    add("encrypted_event_field_mismatch",
        {"event: line",
         "data: {\"line\": \"x\", \"stream\": \"stdout\", \"_conch_event\": \"result\", "
         "\"_conch_sequence\": 0}"},
        true, nullptr);
    add("decrypt_failure_fails_closed",
        {"event: line",
         "data: {\"line\": \"x\", \"stream\": \"stdout\", \"_conch_event\": \"line\", "
         "\"_conch_sequence\": 0}"},
        true, "bad padding");

    add("event_without_data_frame",
        {"event: line", "event: result", "data: {\"exit_code\": 0}"}, false, nullptr);
    add("empty_line_with_pending_event", {"event: line", ""}, false, nullptr);
    {
        std::vector<std::string> lines =
            frames(false, {{"result", frame_data({{"exit_code", number(0)}})}});
        lines.push_back("event: line");
        add("event_after_terminal_frame", std::move(lines), false, nullptr);
    }
    {
        std::vector<std::string> lines =
            frames(false, {{"result", frame_data({{"exit_code", number(0)}})}});
        lines.push_back("data: {}");
        add("data_after_terminal_frame", std::move(lines), false, nullptr);
    }
    add("data_without_event", {"data: {\"line\": \"x\", \"stream\": \"stdout\"}"}, false,
        nullptr);
    add("unsupported_event", {"event: meta", "{}"}, false, nullptr);
    add("unexpected_line", {"this is not sse"}, false, nullptr);
    add("event_without_space_is_unexpected", {"event:line", "data: {}"}, false, nullptr);
    add("stream_ends_with_pending_event", {"event: result"}, false, nullptr);

    add("invalid_json_payload", {"event: line", "data: {oops"}, false, nullptr);
    add("line_event_missing_text", {"event: line", "data: {\"stream\": \"stdout\"}"}, false,
        nullptr);
    add("line_event_missing_stream", {"event: line", "data: {\"line\": \"x\"}"}, false,
        nullptr);
    add("line_event_blank_stream",
        {"event: line", "data: {\"line\": \"x\", \"stream\": \"\"}"}, false, nullptr);
    add("line_event_unsupported_stream",
        {"event: line", "data: {\"line\": \"x\", \"stream\": \"tty\"}"}, false, nullptr);
    add("result_event_missing_exit_code", {"event: result", "data: {}"}, false, nullptr);
    add("result_event_exit_code_is_a_string",
        {"event: result", "data: {\"exit_code\": \"0\"}"}, false, nullptr);
    add("warning_event_missing_message", {"event: warning", "data: {}"}, false, nullptr);
    add("error_event_missing_message",
        {"event: error", "data: {\"timed_out\": true}"}, false, nullptr);
    add("error_event_timeout_is_a_string",
        {"event: error", "data: {\"message\": \"slow\", \"timed_out\": \"true\"}"}, false,
        nullptr);
    add("error_event_timeout_is_not_a_boolean",
        {"event: error", "data: {\"message\": \"slow\", \"timed_out\": \"yes\"}"}, false,
        nullptr);

    add("ascii_output_is_truncated_at_the_byte_budget",
        long_line(std::string(kBudget + 64, 'a')), false, nullptr);
    add("multibyte_character_is_never_split",
        long_line(std::string(kBudget - 2, 'a') + "\u20ac"), false, nullptr);

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

// native-run-archive/gen_scenarios.py — indent=2, ensure_ascii=False.
void profile_native_run_archive(const fs::path& scenarios) {
    struct Overrides {
        const char* status = "COMPLETED";
        json::Value parent_run_id = null_value();
        std::int64_t started_at = 10;
        std::int64_t last_checkpoint_at = 20;
        json::Value stop_requested_at = null_value();
        json::Value ended_at = null_value();
        json::Value end_reason = null_value();
        std::int64_t current_pass = 0;
        bool legacy_ambiguous = false;
    };
    auto snapshot = [](const char* name, const Overrides& overrides) {
        return object({{"name", str(name)},
                       {"op", str("terminalize")},
                       {"id", str("r1")},
                       {"conversationId", str("conv-a")},
                       {"parentRunId", overrides.parent_run_id},
                       {"status", str(overrides.status)},
                       {"startedAt", number(overrides.started_at)},
                       {"lastCheckpointAt", number(overrides.last_checkpoint_at)},
                       {"stopRequestedAt", overrides.stop_requested_at},
                       {"endedAt", overrides.ended_at},
                       {"endReason", overrides.end_reason},
                       {"currentPass", number(overrides.current_pass)},
                       {"legacyAmbiguous", boolean(overrides.legacy_ambiguous)}});
    };
    auto run = [](const char* run_id, json::Value parent, std::int64_t started,
                  const char* conversation, bool ambiguous) {
        return object({{"id", str(run_id)},
                       {"conversationId", str(conversation)},
                       {"parentRunId", std::move(parent)},
                       {"startedAt", number(started)},
                       {"legacyAmbiguous", boolean(ambiguous)}});
    };
    auto ownership = [](const char* message_id, const char* run_id, std::int64_t sequence,
                        const char* conversation, std::optional<std::int64_t> consumed) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"messageId", str(message_id)},
            {"conversationId", str(conversation)},
            {"runId", str(run_id)},
            {"runSequence", number(sequence)},
        };
        if (consumed) members.emplace_back("consumedAtPass", number(*consumed));
        return object(std::move(members));
    };
    auto ownership_case = [](const char* name, std::vector<json::Value> runs,
                             std::vector<json::Value> rows, bool unique) {
        return object({{"name", str(name)},
                       {"op", str("hasCompleteOwnership")},
                       {"sourceRunIdsWereUnique", boolean(unique)},
                       {"runs", array(std::move(runs))},
                       {"ownership", array(std::move(rows))}});
    };
    auto order_case = [](const char* name, std::vector<json::Value> runs) {
        return object({{"name", str(name)},
                       {"op", str("orderByParent")},
                       {"runs", array(std::move(runs))}});
    };

    std::vector<json::Value> cases;
    {
        Overrides overrides;
        overrides.status = "ACTIVE";
        overrides.parent_run_id = str("r-parent");
        overrides.current_pass = 2;
        cases.push_back(snapshot("terminalize_active", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "STOPPING";
        overrides.stop_requested_at = number(15);
        cases.push_back(snapshot("terminalize_stopping", overrides));
    }
    cases.push_back(snapshot("terminalize_completed", Overrides{}));
    {
        Overrides overrides;
        overrides.status = "STOPPED";
        cases.push_back(snapshot("terminalize_stopped", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "FAILED";
        cases.push_back(snapshot("terminalize_failed", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "PAUSED";
        cases.push_back(snapshot("terminalize_invalid_status", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "";
        cases.push_back(snapshot("terminalize_missing_status", overrides));
    }
    {
        Overrides overrides;
        overrides.end_reason = str("DONE");
        cases.push_back(snapshot("terminalize_invalid_end_reason", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "ACTIVE";
        overrides.end_reason = str("USER_STOPPED");
        overrides.ended_at = number(99);
        overrides.legacy_ambiguous = true;
        cases.push_back(snapshot("terminalize_nonterminal_recovery", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "STOPPED";
        overrides.started_at = 100;
        overrides.last_checkpoint_at = 50;
        overrides.stop_requested_at = number(90);
        overrides.ended_at = number(30);
        cases.push_back(snapshot("terminalize_timestamps_clamp", overrides));
    }
    {
        Overrides overrides;
        overrides.ended_at = number(99);
        cases.push_back(snapshot("terminalize_timestamps_preserve", overrides));
    }
    {
        Overrides overrides;
        overrides.current_pass = -5;
        cases.push_back(snapshot("terminalize_pass_clamp", overrides));
    }
    {
        Overrides overrides;
        overrides.status = "NOPE";
        overrides.end_reason = str("USER_STOPPED");
        cases.push_back(snapshot("terminalize_invalid_status_valid_reason", overrides));
    }

    cases.push_back(ownership_case(
        "ownership_valid",
        {run("r1", null_value(), 1, "conv-a", false),
         run("r2", null_value(), 1, "conv-a", false)},
        {ownership("m1", "r1", 0, "conv-a", 0), ownership("m2", "r2", 1, "conv-a", 3)},
        true));
    cases.push_back(ownership_case("ownership_empty_ownership",
                                   {run("r1", null_value(), 1, "conv-a", false)}, {}, true));
    cases.push_back(ownership_case("ownership_empty_runs", {}, {}, true));
    cases.push_back(ownership_case(
        "ownership_duplicate_message",
        {run("r1", null_value(), 1, "conv-a", false),
         run("r2", null_value(), 1, "conv-a", false)},
        {ownership("m1", "r1", 0, "conv-a", std::nullopt),
         ownership("m1", "r2", 1, "conv-a", std::nullopt)},
        true));
    cases.push_back(ownership_case(
        "ownership_duplicate_sequence", {run("r1", null_value(), 1, "conv-a", false)},
        {ownership("m1", "r1", 2, "conv-a", std::nullopt),
         ownership("m2", "r1", 2, "conv-a", std::nullopt)},
        true));
    cases.push_back(ownership_case("ownership_missing_run",
                                   {run("r1", null_value(), 1, "conv-a", false)},
                                   {ownership("m1", "r9", 0, "conv-a", std::nullopt)}, true));
    cases.push_back(ownership_case(
        "ownership_wrong_conversation", {run("r1", null_value(), 1, "conv-a", false)},
        {ownership("m1", "r1", 0, "conv-b", std::nullopt)}, true));
    cases.push_back(ownership_case("ownership_negative_sequence",
                                   {run("r1", null_value(), 1, "conv-a", false)},
                                   {ownership("m1", "r1", -1, "conv-a", std::nullopt)}, true));
    cases.push_back(ownership_case(
        "ownership_null_sequence", {run("r1", null_value(), 1, "conv-a", false)},
        {object({{"messageId", str("m1")},
                 {"conversationId", str("conv-a")},
                 {"runId", str("r1")},
                 {"runSequence", null_value()}})},
        true));
    cases.push_back(ownership_case("ownership_nonunique_source",
                                   {run("r1", null_value(), 1, "conv-a", false)},
                                   {ownership("m1", "r1", 0, "conv-a", std::nullopt)}, false));
    cases.push_back(ownership_case(
        "ownership_duplicate_run_ids_last_wins",
        {run("r1", null_value(), 1, "conv-a", false),
         run("r1", null_value(), 1, "conv-b", false)},
        {ownership("m1", "r1", 0, "conv-b", std::nullopt)}, true));

    cases.push_back(order_case("order_normal_chain",
                               {run("c", str("b"), 30, "conv-a", false),
                                run("b", str("a"), 20, "conv-a", false),
                                run("a", null_value(), 10, "conv-a", false)}));
    cases.push_back(order_case("order_missing_parent",
                               {run("orphan", str("ghost"), 5, "conv-a", false),
                                run("root", null_value(), 1, "conv-a", false),
                                run("orphan-child", str("orphan"), 6, "conv-a", false)}));
    cases.push_back(order_case("order_cycle",
                               {run("a", str("c"), 3, "conv-a", false),
                                run("b", str("a"), 1, "conv-a", false),
                                run("c", str("b"), 2, "conv-a", false)}));
    cases.push_back(order_case("order_cycle_equal_timestamps",
                               {run("a", str("c"), 0, "conv-a", false),
                                run("b", str("a"), 0, "conv-a", false),
                                run("c", str("b"), 0, "conv-a", false)}));
    cases.push_back(order_case("order_duplicate_ids",
                               {run("r1", null_value(), 3, "conv-a", false),
                                run("r1", str("r2"), 1, "conv-a", false),
                                run("r2", null_value(), 2, "conv-a", false),
                                run("c", str("r1"), 4, "conv-a", false)}));
    cases.push_back(order_case("order_duplicate_missing_parent",
                               {run("r1", null_value(), 2, "conv-a", false),
                                run("r1", str("ghost"), 1, "conv-a", false)}));
    cases.push_back(order_case("order_equal_timestamps",
                               {run("b", null_value(), 0, "conv-a", false),
                                run("a", null_value(), 0, "conv-a", false),
                                run("c", str("a"), 0, "conv-a", false)}));
    cases.push_back(order_case("order_empty", {}));

    const FormatOptions pretty{Format::Pretty2, false};
    for (const json::Value& item : cases) {
        std::string name;
        for (const json::Member& member : item.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item, pretty);
    }
    std::cout << "wrote " << cases.size() << " scenarios\n";
}


// migration-18-19/gen_scenarios.py — compact, ensure_ascii=False.
void profile_migration_18_19(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto run = [](const char* rid, json::Value parent, std::int64_t legacy) {
        return object({{"id", str(rid)},
                       {"parentRunId", std::move(parent)},
                       {"legacyAmbiguous", number(legacy)}});
    };
    auto msg = [](const char* mid, json::Value parent, const char* participant,
                  const char* run_id, std::int64_t seq) {
        return object({{"id", str(mid)},
                       {"parentId", std::move(parent)},
                       {"participant", str(participant)},
                       {"runId", str(run_id)},
                       {"runSequence", number(seq)}});
    };
    auto case_add = [&](const char* name, std::vector<json::Value> runs,
                        std::vector<json::Value> messages) {
        all.push_back(object({{"name", str(name)},
                              {"op", str("migrate")},
                              {"runs", array(std::move(runs))},
                              {"messages", array(std::move(messages))}}));
    };
    auto base = [&](const char* name, std::int64_t legacy, const char* canonical_id,
                    const char* canonical_participant, const char* root_parent,
                    std::optional<std::tuple<const char*, const char*, std::int64_t>> dangling) {
        std::vector<json::Value> runs = {run("p", null_value(), 1),
                                         run("r2", str("p"), legacy)};
        std::vector<json::Value> messages = {
            msg(canonical_id, null_value(), canonical_participant, "p", 0),
            msg("root", str(root_parent), "MODEL", "r2", 1),
        };
        if (dangling) {
            messages.push_back(msg(std::get<0>(*dangling), str(std::get<1>(*dangling)), "MODEL",
                                   "r2", std::get<2>(*dangling)));
        }
        case_add(name, std::move(runs), std::move(messages));
    };

    case_add("m18_empty", {}, {});
    case_add("m18_no_repair", {run("p", null_value(), 1)},
             {msg("u", null_value(), "USER", "p", 0)});
    base("m18_repair_one", 1, "u", "USER", "u",
         std::make_tuple("d", "gone", std::int64_t{2}));
    case_add("m18_two_updates_sorted",
             {run("p", null_value(), 1), run("r2", str("p"), 1)},
             {msg("u", null_value(), "USER", "p", 0),
              msg("root", str("u"), "MODEL", "r2", 1),
              msg("d2", str("gone"), "MODEL", "r2", 3),
              msg("d1", str("gone"), "MODEL", "r2", 2)});
    base("m18_legacy_zero", 0, "u", "USER", "u",
         std::make_tuple("d", "gone", std::int64_t{2}));
    case_add("m18_parent_missing", {run("r2", str("px"), 1)},
             {msg("ux", null_value(), "USER", "px", 0),
              msg("root", str("ux"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    case_add("m18_parent_null", {run("r2", null_value(), 1)},
             {msg("u", null_value(), "USER", "p", 0),
              msg("root", str("u"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    case_add("m18_no_canonical_user",
             {run("p", null_value(), 1), run("r2", str("p"), 1)},
             {msg("m0", null_value(), "MODEL", "p", 0),
              msg("root", str("m0"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    case_add("m18_canonical_user_min_id",
             {run("p", null_value(), 1), run("r2", str("p"), 1)},
             {msg("u2", null_value(), "USER", "p", 0),
              msg("u1", null_value(), "USER", "p", 0),
              msg("root", str("u1"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    case_add("m18_canonical_sequence_nonzero",
             {run("p", null_value(), 1), run("r2", str("p"), 1)},
             {msg("u", null_value(), "USER", "p", 2),
              msg("root", str("u"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    base("m18_no_repaired_root", 1, "u", "USER", "other",
         std::make_tuple("d", "gone", std::int64_t{2}));
    case_add("m18_repaired_root_tool",
             {run("p", null_value(), 1), run("r2", str("p"), 1)},
             {msg("u", null_value(), "USER", "p", 0),
              msg("tool_root", str("u"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    base("m18_unknown_participant", 1, "u", "BOGUS", "u",
         std::make_tuple("d", "gone", std::int64_t{2}));
    base("m18_dangling_present_parent", 1, "u", "USER", "u",
         std::make_tuple("d", "root", std::int64_t{2}));
    base("m18_dangling_tool_prefix", 1, "u", "USER", "u",
         std::make_tuple("tool_d", "gone", std::int64_t{2}));
    case_add("m18_duplicate_dangling_id",
             {run("pA", null_value(), 1), run("pB", null_value(), 1),
              run("rA", str("pA"), 1), run("rB", str("pB"), 1)},
             {msg("uA", null_value(), "USER", "pA", 0),
              msg("uB", null_value(), "USER", "pB", 0),
              msg("rootA", str("uA"), "MODEL", "rA", 0),
              msg("rootB", str("uB"), "MODEL", "rB", 0),
              msg("d", str("gone"), "MODEL", "rA", 1),
              msg("d", str("gone"), "MODEL", "rB", 1)});
    case_add("m18_multiple_runs_order",
             {run("p", null_value(), 1), run("r3", str("p"), 1), run("r2", str("p"), 1)},
             {msg("u", null_value(), "USER", "p", 0),
              msg("d3", str("gone"), "MODEL", "r3", 1),
              msg("root3", str("u"), "MODEL", "r3", 2),
              msg("root2", str("u"), "MODEL", "r2", 1),
              msg("d2", str("gone"), "MODEL", "r2", 2)});
    base("m18_legacy_two", 2, "u", "USER", "u",
         std::make_tuple("d", "gone", std::int64_t{2}));
    base("m18_legacy_negative", -1, "u", "USER", "u",
         std::make_tuple("d", "gone", std::int64_t{2}));
    case_add("m18_self_parent",
             {run("p", null_value(), 1), run("r2", str("r2"), 1)},
             {msg("u", null_value(), "USER", "p", 0),
              msg("root", str("u"), "MODEL", "r2", 1),
              msg("d", str("gone"), "MODEL", "r2", 2)});
    case_add("m18_runs_without_messages",
             {run("p", null_value(), 1), run("r2", str("p"), 1)}, {});

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

// streaming-json-document/gen_scenarios.py — compact, ensure_ascii=False.
void profile_streaming_json_document(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto parse = [&](const char* name, const char* source) {
        all.push_back(object(
            {{"name", str(name)}, {"op", str("parse")}, {"source", str(source)}}));
    };
    auto hints = [&](const char* name, const char* kind, json::Value raw) {
        all.push_back(object({{"name", str(name)},
                              {"op", str("hints")},
                              {"kind", str(kind)},
                              {"rawArguments", std::move(raw)}}));
    };
    auto normalize = [&](const char* name, json::Value value, std::int64_t max_chars) {
        all.push_back(object({{"name", str(name)},
                              {"op", str("normalize")},
                              {"value", std::move(value)},
                              {"max", number(max_chars)}}));
    };

    parse("doc_empty", "");
    parse("doc_space_only", " ");
    parse("doc_ws_only_multichar", "\t\n ");
    parse("doc_ws_then_value", "  42  ");
    parse("doc_crlf_whitespace", "{\r\n\"a\":\r\n1\r\n}");
    parse("doc_object_complete", R"json({"a":1})json");
    parse("doc_object_empty", "{}");
    parse("doc_object_empty_trailing_ws", "{} ");
    parse("doc_object_partial_key", R"json({")json");
    parse("doc_object_partial_value_null", R"json({"a":)json");
    parse("doc_object_partial_value", R"json({"a":1)json");
    parse("doc_object_after_comma", R"json({"a":1,)json");
    parse("doc_object_trailing_comma", R"json({"a":1,})json");
    parse("doc_object_key_not_string", "{a:1}");
    parse("doc_object_no_colon", R"json({"a" 1})json");
    parse("doc_object_missing_comma", R"json({"a":1 "b":2})json");
    parse("doc_object_bad_after_value", R"json({"a":1 2})json");
    parse("doc_object_key_with_escape", R"json({"a\"b":1})json");
    parse("doc_object_key_only", R"json({"a")json");
    parse("doc_object_nested_partial", R"json({"a":{"b":)json");
    parse("doc_object_keyword_value_partial", R"json({"a":nul)json");
    parse("doc_object_deep_nest", R"json([1,{"b":[2,{"c":)json");
    parse("doc_array_complete", "[1,2,3]");
    parse("doc_array_empty", "[]");
    parse("doc_array_after_comma", "[1,");
    parse("doc_array_trailing_comma", "[1,]");
    parse("doc_array_missing_comma", "[1 2]");
    parse("doc_array_mixed", R"json([1,"a",true,null,{},[]])json");
    parse("doc_array_nested_partial", R"json([["a)json");
    parse("doc_string_root", R"json("abc")json");
    parse("doc_string_partial", R"json("abc)json");
    parse("doc_string_empty", R"json("")json");
    parse("doc_string_escapes", R"json("\"\\\/\b\f\n\r\t")json");
    parse("doc_string_unicode_escape", R"json("\u0041\u00e9")json");
    parse("doc_string_unicode_surrogate_pair", R"json("\ud83d\ude42")json");
    parse("doc_string_trailing_backslash", R"json("abc\)json");
    parse("doc_string_bad_escape", R"json("\q")json");
    parse("doc_string_bad_unicode", R"json("\u12g4")json");
    parse("doc_string_short_unicode_truncated", R"json("\u12)json");
    parse("doc_string_control_char", "{\"a\":\"x\x01y\"}");
    parse("doc_string_non_ascii", "\"h\u00e9llo \U0001f642\"");
    parse("doc_error_offset_after_emoji", "\"\U0001f642\\q\"");
    parse("doc_error_offset_after_accent", "\"\u00e9\\q\"");
    parse("doc_keyword_true", "true");
    parse("doc_keyword_false", "false");
    parse("doc_keyword_null", "null");
    parse("doc_keyword_partial", "tru");
    parse("doc_keyword_mismatch", "trX");
    parse("doc_number_int", "42");
    parse("doc_number_negative", "-42");
    parse("doc_number_fraction", "4.25");
    parse("doc_number_exponent", "1e10");
    parse("doc_number_exponent_sign", "-0.5e-10");
    parse("doc_number_partial_fraction", "12.");
    parse("doc_number_partial_exponent", "1e");
    parse("doc_number_partial_exponent_sign", "1e+");
    parse("doc_number_leading_zero", "01");
    parse("doc_number_bad_fraction", "1.x");
    parse("doc_number_bad_exponent", "1ex");
    parse("doc_number_minus_only", "-");
    parse("doc_number_minus_garbage", "-x");
    parse("doc_multi_roots_ws", "1 2");
    parse("doc_multi_roots_cr", "1\r\n2");
    parse("doc_multi_roots_ws_objects", "{} []");
    parse("doc_multi_roots_no_ws", "{}[]");
    parse("doc_multi_roots_then_garbage", "{} x");
    parse("doc_bom_not_whitespace", "\ufeff{}");

    hints("hints_memory_read_single", "MEMORY_READ", str(R"json({"names":["note.md"]})json"));
    hints("hints_memory_read_partial_name", "MEMORY_READ", str(R"json({"names":["no)json"));
    hints("hints_memory_read_two_names", "MEMORY_READ",
          str(R"json({"names":["a.md","b.md"]})json"));
    hints("hints_memory_read_empty_name", "MEMORY_READ", str(R"json({"names":[""]})json"));
    hints("hints_memory_read_blank_name", "MEMORY_READ", str(R"json({"names":["  "]})json"));
    hints("hints_memory_read_empty_array", "MEMORY_READ", str(R"json({"names":[]})json"));
    hints("hints_memory_read_names_truncated", "MEMORY_READ", str(R"json({"names":[)json"));
    hints("hints_memory_read_fallback_name", "MEMORY_READ", str(R"json({"name":"x.md"})json"));
    hints("hints_skill_read_single", "SKILL_READ", str(R"json({"names":["skill.md"]})json"));
    hints("hints_skill_read_blank", "SKILL_READ", str(R"json({"names":[" "]})json"));
    hints("hints_memory_list", "MEMORY_LIST", str(R"json({"names":["a","b"]})json"));
    hints("hints_skill_list", "SKILL_LIST", str(R"json({"names":["s1"]})json"));
    hints("hints_memory_create", "MEMORY_CREATE",
          str(R"json({"name":"x.md","content":"hi"})json"));
    hints("hints_memory_edit", "MEMORY_EDIT", str(R"json({"name":"y.md"})json"));
    hints("hints_memory_delete", "MEMORY_DELETE", str(R"json({"name":"z.md"})json"));
    hints("hints_skill_create", "SKILL_CREATE", str(R"json({"name":"s.md"})json"));
    hints("hints_skill_edit", "SKILL_EDIT", str(R"json({"name":"s2.md"})json"));
    hints("hints_skill_delete", "SKILL_DELETE", str(R"json({"name":"s3.md"})json"));
    hints("hints_web_search", "WEB_SEARCH", str(R"json({"query":"cats"})json"));
    hints("hints_conversation_search", "CONVERSATION_SEARCH",
          str(R"json({"query":"hello"})json"));
    hints("hints_web_fetch", "WEB_FETCH", str(R"json({"url":"https://example.com"})json"));
    hints("hints_conversation_read", "CONVERSATION_READ",
          str(R"json({"conversation_id":"c7"})json"));
    hints("hints_shell_execute", "SHELL_EXECUTE",
          str(R"json({"command":"ls -la","cwd":"/tmp"})json"));
    hints("hints_shell_job_get", "SHELL_JOB_GET", str(R"json({"job_id":"j1"})json"));
    hints("hints_shell_job_wait", "SHELL_JOB_WAIT", str(R"json({"job_id":"j2"})json"));
    hints("hints_shell_job_stop", "SHELL_JOB_STOP", str(R"json({"job_id":"j3"})json"));
    hints("hints_file_read", "FILE_READ", str(R"json({"path":"/etc/hosts"})json"));
    hints("hints_file_write", "FILE_WRITE", str(R"json({"path":"/tmp/x.txt"})json"));
    hints("hints_file_edit", "FILE_EDIT", str(R"json({"path":"a/b.kt"})json"));
    hints("hints_image_view", "IMAGE_VIEW", str(R"json({"path":"img.png"})json"));
    hints("hints_file_glob", "FILE_GLOB", str(R"json({"pattern":"**/*.kt"})json"));
    hints("hints_file_grep", "FILE_GREP", str(R"json({"pattern":"TODO"})json"));
    hints("hints_image_generate", "IMAGE_GENERATE", str(R"json({"prompt":"a cat"})json"));
    hints("hints_task_create", "TASK_CREATE", str(R"json({"name":"task one"})json"));
    hints("hints_task_delete_id", "TASK_DELETE", str(R"json({"id_or_name":"t1"})json"));
    hints("hints_task_delete_name", "TASK_DELETE", str(R"json({"name":"t2"})json"));
    hints("hints_task_delete_task_id", "TASK_DELETE", str(R"json({"task_id":"t3"})json"));
    hints("hints_task_delete_none", "TASK_DELETE", str(R"json({"other":1})json"));
    hints("hints_task_list", "TASK_LIST", str(R"json({"names":["a"]})json"));
    hints("hints_loop_start", "LOOP_START", str(R"json({"name":"x"})json"));
    hints("hints_ask_user", "ASK_USER", str(R"json({"question":"q?"})json"));
    hints("hints_unknown_kind", "UNKNOWN", str(R"json({"name":"x"})json"));
    hints("hints_server_operation_destination", "MEMORY_CREATE",
          str(R"json({"name":"a","server":"srv","operation":"PUT","new_name":"b"})json"));
    hints("hints_operation_shell", "SHELL_EXECUTE",
          str(R"json({"command":"pwd","operation":"run"})json"));
    hints("hints_raw_null", "FILE_READ", null_value());
    hints("hints_raw_blank", "FILE_READ", str("   "));
    hints("hints_raw_not_json", "FILE_READ", str("hello world"));
    hints("hints_raw_open_brace", "FILE_READ", str("{"));
    hints("hints_prefix_fits", "MEMORY_CREATE",
          str("{\"name\":\"" + std::string(4084, 'a') + "\"}"));
    hints("hints_prefix_cut_mid_string", "MEMORY_CREATE",
          str("{\"name\":\"" + std::string(4086, 'a') + "\"}"));
    hints("hints_prefix_boundary_excludes_tail", "MEMORY_CREATE",
          str("{\"name\":\"x\",\"content\":\"" + std::string(4071, 'A') +
              "\",\"new_name\":\"dst\"}"));
    hints("hints_prefix_boundary_includes_tail", "MEMORY_CREATE",
          str("{\"name\":\"x\",\"content\":\"" + std::string(4055, 'A') +
              "\",\"new_name\":\"dst\"}"));

    normalize("normalize_null", null_value(), 120);
    normalize("normalize_empty", str(""), 120);
    normalize("normalize_blank", str("   "), 120);
    normalize("normalize_collapse", str("  a b  "), 120);
    normalize("normalize_newline_tab", str("a\n\tb   c"), 120);
    normalize("normalize_nbsp_trimmed", str("\u00a0x\u00a0"), 120);
    normalize("normalize_nbsp_only", str("\u00a0"), 120);
    normalize("normalize_vertical_tab_form_feed", str("a\x0b" "b\f" "c"), 120);
    normalize("normalize_exact_120", str(std::string(120, 'x')), 120);
    normalize("normalize_over_120", str(std::string(121, 'x')), 120);
    {
        std::string value;
        for (int i = 0; i < 59; ++i) value += "\U0001f642";
        normalize("normalize_unicode_length_120", str(value + "xy"), 120);
        normalize("normalize_unicode_length_121", str(value + "xyz"), 120);
    }
    normalize("normalize_small_max", str("abcdefg"), 5);
    normalize("normalize_single_char_max", str("ab"), 1);

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


// message-source/gen_scenarios.py — compact, ensure_ascii=False.
void profile_message_source(const fs::path& scenarios) {
    std::vector<json::Value> all;
    auto case_add = [&](const char* name,
                        std::vector<std::pair<std::string, json::Value>> fields) {
        std::vector<std::pair<std::string, json::Value>> members = {{"name", str(name)}};
        for (auto& field : fields) members.push_back(std::move(field));
        all.push_back(object(std::move(members)));
    };
    auto decode = [&](const char* name, json::Value raw) {
        case_add(name, {{"op", str("decode")}, {"raw", std::move(raw)}});
    };
    auto encode = [&](const char* name, json::Value raw) {
        case_add(name, {{"op", str("encode")}, {"raw", std::move(raw)}});
    };
    auto sanitize = [&](const char* name, json::Value raw, const char* participant) {
        case_add(name,
                 {{"op", str("sanitize")},
                  {"raw", std::move(raw)},
                  {"participant", str(participant)}});
    };
    auto readable = [&](const char* name, std::vector<json::Value> items) {
        case_add(name, {{"op", str("readable")}, {"items", array(std::move(items))}});
    };
    auto automation = [&](const char* name, json::Value request_kind) {
        case_add(name,
                 {{"op", str("automation")}, {"requestKind", std::move(request_kind)}});
    };
    auto stored = [](const char* kind,
                     std::optional<std::vector<std::pair<std::string, std::optional<std::string>>>>
                         items) {
        std::vector<std::pair<std::string, json::Value>> members = {{"kind", str(kind)}};
        if (items) {
            std::vector<json::Value> rows;
            for (auto& [question, answer] : *items) {
                std::vector<std::pair<std::string, json::Value>> entry = {
                    {"question", str(question)}};
                if (answer) entry.emplace_back("answer", str(*answer));
                rows.push_back(object(std::move(entry)));
            }
            members.emplace_back("askUser", array(std::move(rows)));
        }
        return serialize(object(std::move(members)), FormatOptions{Format::Compact, false});
    };
    auto qa = [&](const char* question, std::optional<std::string> answer) {
        return std::make_pair(std::string(question), std::move(answer));
    };
    auto item = [&](const char* question, std::optional<std::string> answer) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"question", str(question)}};
        if (answer) members.emplace_back("answer", str(*answer));
        return object(std::move(members));
    };

    decode("decode_null", null_value());
    decode("decode_empty", str(""));
    decode("decode_blank", str("   "));
    decode("decode_task", str(R"json({"kind":"task"})json"));
    decode("decode_loop", str(R"json({"kind":"loop"})json"));
    decode("decode_ask_user_empty", str(R"json({"kind":"ask_user"})json"));
    decode("decode_ask_user_one", str(stored("ask_user", std::vector<std::pair<
                                                           std::string, std::optional<std::string>>>{
                                                       qa("What now?", std::string("Continue"))})));
    decode("decode_ask_user_two",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("Q1", std::string("A1")), qa("Q2", std::string("A2"))})));
    decode("decode_answer_missing",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("Q", std::nullopt)})));
    decode("decode_answer_null",
           str(R"json({"kind":"ask_user","askUser":[{"question":"Q","answer":null}]})json"));
    decode("decode_answer_empty",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("Q", std::string(""))})));
    decode("decode_question_empty",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("", std::string("A"))})));
    decode("decode_unknown_top_key", str(R"json({"kind":"task","x":1})json"));
    decode("decode_unknown_nested_key",
           str(R"json({"kind":"ask_user","askUser":[{"question":"Q","answer":"A","x":[1,2]}]})json"));
    decode("decode_serial_task_upper", str(R"json({"kind":"TASK"})json"));
    decode("decode_kind_missing", str(R"json({"x":1})json"));
    decode("decode_kind_null", str(R"json({"kind":null})json"));
    decode("decode_kind_number", str(R"json({"kind":3})json"));
    decode("decode_kind_unknown", str(R"json({"kind":"bogus"})json"));
    decode("decode_ask_user_null", str(R"json({"kind":"ask_user","askUser":null})json"));
    decode("decode_ask_user_object", str(R"json({"kind":"ask_user","askUser":{}})json"));
    decode("decode_item_not_object", str(R"json({"kind":"ask_user","askUser":[1]})json"));
    decode("decode_item_question_missing",
           str(R"json({"kind":"ask_user","askUser":[{"answer":"A"}]})json"));
    decode("decode_item_question_number",
           str(R"json({"kind":"ask_user","askUser":[{"question":5}]})json"));
    decode("decode_item_question_null",
           str(R"json({"kind":"ask_user","askUser":[{"question":null}]})json"));
    decode("decode_item_answer_number",
           str(R"json({"kind":"ask_user","askUser":[{"question":"Q","answer":5}]})json"));
    decode("decode_item_answer_bool",
           str(R"json({"kind":"ask_user","askUser":[{"question":"Q","answer":true}]})json"));
    decode("decode_task_with_items",
           str(stored("task", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                  qa("What now?", std::string("Continue"))})));
    decode("decode_loop_with_items",
           str(stored("loop", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                  qa("What now?", std::string("Continue"))})));
    decode("decode_extra_whitespace", str(R"json(  { "kind" : "task" }  )json"));
    decode("decode_trailing_junk", str(R"json({"kind":"task"}x)json"));
    decode("decode_two_values", str(R"json({"kind":"task"}{"kind":"loop"})json"));
    decode("decode_bom", str("\ufeff{\"kind\":\"task\"}"));
    decode("decode_escaped_text",
           str(stored("ask_user", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                      qa("a\"b\\c\nd\te", std::string("\u00e9\U0001f642"))})));
    decode("decode_literal_control",
           str("{\"kind\":\"ask_user\",\"askUser\":[{\"question\":\"a\x01" "b\"}]}"));
    decode("decode_task_empty_items", str(R"json({"kind":"task","askUser":[]})json"));
    decode("decode_single_quotes", str("{'kind':'task'}"));
    decode("decode_unclosed", str(R"json({"kind":"task")json"));
    decode("decode_deep_nested_unknown",
           str(R"json({"kind":"loop","meta":{"a":[1,{"b":null}]}})json"));

    encode("encode_null", null_value());
    encode("encode_invalid", str("not json"));
    encode("encode_task", str(R"json({"kind":"task"})json"));
    encode("encode_loop", str(R"json({"kind":"loop"})json"));
    encode("encode_ask_user_empty", str(R"json({"kind":"ask_user"})json"));
    encode("encode_ask_user_one", str(stored("ask_user", std::vector<std::pair<
                                                           std::string, std::optional<std::string>>>{
                                                       qa("What now?", std::string("Continue"))})));
    encode("encode_answer_missing",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("Q", std::nullopt)})));
    encode("encode_answer_null",
           str(R"json({"kind":"ask_user","askUser":[{"question":"Q","answer":null}]})json"));
    encode("encode_answer_empty",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("Q", std::string(""))})));
    encode("encode_question_escapes",
           str(stored("ask_user", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                      qa("a\"b\\c\nd\te", std::string("tab\there"))})));
    encode("encode_unicode",
           str(stored("ask_user", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                      qa("h\u00e9llo \U0001f642",
                                         std::string("\u65e5\u672c\u8a9e"))})));
    encode("encode_control_chars",
           str(stored("ask_user", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                      qa("\x01\x07\x0b\x1f", std::string("ok"))})));
    encode("encode_input_whitespace",
           str(R"json( { "kind" : "ask_user" , "askUser" : [ { "question" : "Q" } ] } )json"));
    encode("encode_task_empty_items", str(R"json({"kind":"task","askUser":[]})json"));
    encode("encode_unknown_keys", str(R"json({"kind":"task","x":1,"askUser":[]})json"));
    encode("encode_multi_items",
           str(stored("ask_user",
                      std::vector<std::pair<std::string, std::optional<std::string>>>{
                          qa("Q1", std::string("A1")), qa("Q2", std::nullopt),
                          qa("Q3", std::string(""))})));
    encode("encode_task_with_items",
           str(stored("task", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                  qa("What now?", std::string("Continue"))})));
    encode("encode_question_only_spaces",
           str(stored("ask_user", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                      qa("  ", std::nullopt)})));

    sanitize("sanitize_user_task", str(R"json({"kind":"task"})json"), "USER");
    sanitize("sanitize_user_invalid", str("not json"), "USER");
    sanitize("sanitize_user_null", null_value(), "USER");
    sanitize("sanitize_user_blank", str("  "), "USER");
    sanitize("sanitize_model_task", str(R"json({"kind":"task"})json"), "MODEL");
    sanitize("sanitize_error_task", str(R"json({"kind":"task"})json"), "ERROR");
    sanitize("sanitize_user_ask_user",
             str(stored("ask_user", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                        qa("What now?", std::string("Continue"))})),
             "USER");
    sanitize("sanitize_user_ask_user_empty", str(R"json({"kind":"ask_user","askUser":[]})json"),
             "USER");
    sanitize("sanitize_user_unknown_keys", str(R"json({"kind":"loop","x":1})json"), "USER");
    sanitize("sanitize_user_task_with_items",
             str(stored("task", std::vector<std::pair<std::string, std::optional<std::string>>>{
                                    qa("What now?", std::string("Continue"))})),
             "USER");
    sanitize("sanitize_user_padded", str(R"json(  {"kind":"task"}  )json"), "USER");

    readable("readable_one", {item("Q", std::string("A"))});
    readable("readable_answer_missing", {item("Q", std::nullopt)});
    readable("readable_answer_empty", {item("Q", std::string(""))});
    readable("readable_two", {item("Q1", std::string("A1")), item("Q2", std::string("A2"))});
    readable("readable_three",
             {item("Q1", std::string("A1")), item("Q2", std::nullopt),
              item("Q3", std::string("A3"))});
    readable("readable_question_newline", {item("line1\nline2", std::string("A"))});
    readable("readable_question_blank", {item("", std::string("A"))});
    readable("readable_answer_newline", {item("Q", std::string("a\nb"))});
    readable("readable_emoji", {item("\U0001f642?", std::string("\u00e9"))});
    readable("readable_pipes", {item("a|b", std::string("c;d"))});

    automation("automation_task", str("task"));
    automation("automation_loop", str("loop"));
    automation("automation_other", str("other"));
    automation("automation_empty", str(""));
    automation("automation_null", null_value());
    automation("automation_uppercase", str("TASK"));
    automation("automation_padded", str(" task "));
    case_add("constants", {{"op", str("constants")}});

    remove_stale(scenarios);
    const FormatOptions compact{Format::Compact, false};
    for (const json::Value& item_value : all) {
        std::string name;
        for (const json::Member& member : item_value.members) {
            if (member.key == "name") name = member.value.text;
        }
        write_scenario(scenarios, name, item_value, compact);
    }
    std::cout << "wrote " << all.size() << " scenarios to " << scenarios.string() << "\n";
}

// ---------------------------------------------------------------------------------------------
// zip-reader/gen_scenarios.py — indent=2, ensure_ascii=False; the raw archives need zlib's raw
// deflate and CPython's Mersenne Twister (random.Random(9000)) to reproduce byte-for-byte.

class PythonRandom {
  public:
    explicit PythonRandom(std::uint32_t seed) {
        const std::uint32_t key[1] = {seed};
        init_by_array(key, 1);
    }

    std::uint32_t randbelow(std::uint32_t n) {
        if (n == 0) return 0;
        int bits = 0;
        for (std::uint32_t value = n; value != 0; value >>= 1) ++bits;
        std::uint32_t result = getrandbits(bits);
        while (result >= n) result = getrandbits(bits);
        return result;
    }

    double random() {
        const std::uint32_t a = genrand_uint32() >> 5;
        const std::uint32_t b = genrand_uint32() >> 6;
        return (static_cast<double>(a) * 67108864.0 + static_cast<double>(b)) *
               (1.0 / 9007199254740992.0);
    }

  private:
    static constexpr int kSize = 624;

    std::uint32_t getrandbits(int bits) { return genrand_uint32() >> (32 - bits); }

    void init_genrand(std::uint32_t seed) {
        mt_[0] = seed;
        for (int i = 1; i < kSize; ++i) {
            mt_[i] = 1812433253U * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) +
                     static_cast<std::uint32_t>(i);
        }
        index_ = kSize;
    }

    void init_by_array(const std::uint32_t* key, int key_length) {
        init_genrand(19650218U);
        int i = 1;
        int j = 0;
        int k = kSize > key_length ? kSize : key_length;
        for (; k != 0; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] +
                     static_cast<std::uint32_t>(j);
            ++i;
            ++j;
            if (i >= kSize) {
                mt_[0] = mt_[kSize - 1];
                i = 1;
            }
            if (j >= key_length) j = 0;
        }
        for (k = kSize - 1; k != 0; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) -
                     static_cast<std::uint32_t>(i);
            ++i;
            if (i >= kSize) {
                mt_[0] = mt_[kSize - 1];
                i = 1;
            }
        }
        mt_[0] = 0x80000000U;
    }

    std::uint32_t genrand_uint32() {
        if (index_ >= kSize) {
            for (int i = 0; i < kSize; ++i) {
                const std::uint32_t y =
                    (mt_[i] & 0x80000000U) | (mt_[(i + 1) % kSize] & 0x7FFFFFFFU);
                mt_[i] = mt_[(i + 397) % kSize] ^ (y >> 1);
                if ((y & 1U) != 0) mt_[i] ^= 0x9908B0DFU;
            }
            index_ = 0;
        }
        std::uint32_t y = mt_[index_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9D2C5680U;
        y ^= (y << 15) & 0xEFC60000U;
        y ^= y >> 18;
        return y;
    }

    std::uint32_t mt_[kSize];
    int index_ = kSize;
};

std::string raw_deflate(const std::string& payload) {
    z_stream stream{};
    if (deflateInit2(&stream, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        fail("error: deflateInit2 failed");
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.data()));
    stream.avail_in = static_cast<uInt>(payload.size());
    std::string output;
    char buffer[16384];
    int result = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        result = deflate(&stream, Z_FINISH);
        output.append(buffer, sizeof(buffer) - stream.avail_out);
    } while (result == Z_OK);
    deflateEnd(&stream);
    if (result != Z_STREAM_END) fail("error: deflate failed");
    return output;
}

std::string hex_encode(const std::string& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0xF]);
        out.push_back(kDigits[byte & 0xF]);
    }
    return out;
}

void append_le16(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void append_le32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::uint32_t crc32_of(const std::string& data) {
    return static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
}

std::string local_header(
    const std::string& name, const std::string& data, std::uint32_t method = 0,
    std::uint32_t flags = 0, std::optional<std::uint32_t> crc = std::nullopt,
    std::optional<std::uint32_t> size = std::nullopt,
    std::optional<std::uint32_t> compressed_size = std::nullopt,
    const std::string& extra = std::string(),
    std::optional<std::uint32_t> descriptor_crc = std::nullopt,
    std::optional<std::uint32_t> descriptor_size = std::nullopt) {
    const std::uint32_t crc_value = crc ? *crc : crc32_of(data);
    const std::uint32_t size_value =
        size ? *size : static_cast<std::uint32_t>(data.size());
    const std::uint32_t compressed_value =
        compressed_size ? *compressed_size : static_cast<std::uint32_t>(data.size());
    std::string header;
    append_le32(header, 0x04034B50U);
    append_le16(header, 20);
    append_le16(header, flags);
    append_le16(header, method);
    append_le16(header, 0);
    append_le16(header, 0);
    append_le32(header, crc_value);
    append_le32(header, compressed_value);
    append_le32(header, size_value);
    append_le16(header, static_cast<std::uint32_t>(name.size()));
    append_le16(header, static_cast<std::uint32_t>(extra.size()));
    header += name;
    header += extra;
    if (!descriptor_crc && !descriptor_size && (flags & 0x08U) == 0) {
        return header + data;
    }
    std::string descriptor;
    append_le32(descriptor, descriptor_crc ? *descriptor_crc : crc_value);
    append_le32(descriptor,
                descriptor_size ? *descriptor_size
                                : static_cast<std::uint32_t>(data.size()));
    append_le32(descriptor, size_value);
    return header + data + descriptor;
}

void profile_zip_reader(const fs::path& scenarios) {
    std::vector<std::pair<std::string, json::Value>> entries;
    auto entry = [](const char* name, const std::string& content, const char* method,
                    const char* extra) {
        std::vector<std::pair<std::string, json::Value>> members = {
            {"name", str(name)}, {"content", str(content)}, {"method", str(method)}};
        if (extra != nullptr) members.emplace_back("extra", str(extra));
        return object(std::move(members));
    };
    auto write_entries = [&](const char* name, std::vector<json::Value> items) {
        entries.emplace_back(
            name, object({{"entries", array(std::move(items))}}));
    };
    auto write_raw = [&](const char* name, const std::string& bytes) {
        entries.emplace_back(name, object({{"raw", str(hex_encode(bytes))}}));
    };

    write_entries("01-single-deflate",
                  {entry("conversations.json", R"json({"conversations":[]})json", "deflate",
                         nullptr)});
    write_entries("02-multiple-entries",
                  {entry("a.json", R"json({"a":1})json", "deflate", nullptr),
                   entry("b/c.json", R"json({"b":2})json", "stored", nullptr),
                   entry("d.txt", "plain text", "stored", nullptr)});
    write_entries("03-empty-entry", {entry("empty.json", "", "deflate", nullptr)});
    write_entries("04-utf8-name-and-content",
                  {entry("\u0434\u0438\u0430\u043b\u043e\u0433\u0438/\u65e5\u672c\u8a9e.json",
                         R"json({"title":"юникод 🙂"})json", "deflate", nullptr)});
    write_entries("05-binary-content",
                  {entry("bin.dat", std::string("\x00\x01\x02\u00ff", 5), "deflate",
                         nullptr)});
    write_entries("06-extra-field",
                  {entry("with-extra.json", R"json({"x":1})json", "deflate",
                         "EXTRA-INFO-BYTES")});
    {
        std::vector<json::Value> items;
        for (int index = 0; index < 20; ++index) {
            items.push_back(entry(("part-" + std::to_string(index) + ".json").c_str(),
                                  ("{\"i\":" + std::to_string(index) + "}").c_str(), "deflate",
                                  nullptr));
        }
        write_entries("07-many-entries", std::move(items));
    }
    {
        std::string content;
        for (int i = 0; i < 20000; ++i) content += "abcdefgh";
        write_entries("08-large-repetitive",
                      {entry("big.txt", content.c_str(), "deflate", nullptr)});
    }
    {
        std::string content;
        for (int index = 0; index < 20000; ++index) {
            const int code_point = 0x4E00 + (index * 7919) % 0x1000;
            content += static_cast<char>(0xE0 | (code_point >> 12));
            content += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            content += static_cast<char>(0x80 | (code_point & 0x3F));
        }
        write_entries("09-large-incompressible",
                      {entry("noise.bin", content.c_str(), "deflate", nullptr)});
    }
    write_entries("10-mixed-methods",
                  {entry("one.json", R"json({"one":1})json", "stored", nullptr),
                   entry("two.json", R"json({"two":2})json", "deflate", nullptr),
                   entry("three.json", R"json({"three":3})json", "stored", nullptr)});
    write_entries("11-single-byte-content", {entry("tiny.txt", "x", "deflate", nullptr)});
    write_entries("12-tab-and-newline-content",
                  {entry("lines.txt", "a\tb\nc\rd\n", "deflate", nullptr)});

    const std::string payload = R"json({"raw":true})json";
    write_raw("20-raw-valid-stored", local_header("a.json", payload));
    {
        std::string broken = std::string("PK\x03\x05", 4) + local_header("a.json", payload).substr(4);
        write_raw("21-raw-bad-signature", broken);
    }
    write_raw("22-raw-bad-method", local_header("a.json", payload, 12));
    {
        const std::string full = local_header("a.json", payload);
        write_raw("23-raw-truncated", full.substr(0, full.size() - 4));
    }
    write_raw("24-raw-crc-mismatch", local_header("a.json", payload, 0, 0, 0xDEADBEEFU));
    write_raw("25-raw-size-mismatch",
              local_header("a.json", payload, 0, 0, std::nullopt,
                           static_cast<std::uint32_t>(payload.size() + 5)));
    write_raw("26-raw-garbage", "not a zip at all, just bytes");
    write_raw("27-raw-empty", "");
    {
        std::string central;
        append_le32(central, 0x02014B50U);
        append_le16(central, 20);
        append_le16(central, 20);
        for (int i = 0; i < 4; ++i) append_le16(central, 0);
        append_le32(central, 0);
        append_le32(central, 0);
        append_le32(central, 0);
        for (int i = 0; i < 5; ++i) append_le16(central, 0);
        append_le32(central, 0);
        append_le32(central, 0);
        write_raw("28-raw-central-directory-only", central);
    }
    {
        std::string repeated;
        for (int i = 0; i < 40; ++i) repeated += payload;
        write_raw("29-raw-damaged-deflate", local_header("a.json", raw_deflate(repeated), 8));
    }
    {
        // _damaged_deflate(payload * 40) multiplies by another 40 internally.
        std::string compressed;
        for (int i = 0; i < 1600; ++i) compressed += payload;
        std::string data = raw_deflate(compressed);
        if (data.size() > 6) data[6] = static_cast<char>(data[6] ^ 0xFF);
        write_raw("30-raw-damaged-deflate-byte", local_header("a.json", data, 8));
    }
    write_raw("31-raw-descriptor-wrong-crc",
              local_header("a.json", payload, 0, 0x08, std::nullopt, std::nullopt,
                           std::nullopt, std::string(), 0x12345678U));
    write_raw("32-raw-descriptor-wrong-size",
              local_header("a.json", payload, 0, 0x08, std::nullopt, std::nullopt,
                           std::nullopt, std::string(), std::nullopt, 999U));
    write_raw("33-raw-directory-entry", local_header("dir/", ""));
    write_raw("34-raw-invalid-utf8-content",
              local_header("a.bin", std::string("\xff\xfe\x00" "abc", 6)));
    write_raw("35-raw-zero-length-name", local_header("", payload));
    write_raw("36-raw-deflate-empty", local_header("a.json", raw_deflate(""), 8));
    write_raw("37-raw-stored-empty", local_header("a.json", ""));
    write_raw("38-raw-two-entries-second-broken",
              local_header("a.json", payload) + local_header("b.json", payload, 0, 0, 1U));

    PythonRandom rng(9000);
    for (int index = 0; index < 8; ++index) {
        std::string archive;
        const int chunk_count = 1 + static_cast<int>(rng.randbelow(3));
        for (int chunk = 0; chunk < chunk_count; ++chunk) {
            const std::string name =
                "f" + std::to_string(rng.randbelow(10)) + ".bin";
            const int size = static_cast<int>(rng.randbelow(65));
            std::string data;
            for (int byte = 0; byte < size; ++byte) {
                data.push_back(static_cast<char>(rng.randbelow(256)));
            }
            const bool deflated = rng.randbelow(2) == 1;
            std::string header;
            if (deflated) {
                header = local_header(name, raw_deflate(data), 8);
            } else {
                header = local_header(name, data, 0);
            }
            if (rng.random() < 0.3) {
                header[4] = static_cast<char>(rng.randbelow(256));
            }
            archive += header;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "90-fuzz-%02d", index);
        write_raw(name, archive);
    }

    remove_stale(scenarios);
    const FormatOptions pretty{Format::Pretty2, false};
    for (const auto& [name, value] : entries) {
        write_scenario(scenarios, name, value, pretty);
    }
    std::cout << "Wrote " << entries.size() << " scenarios to " << scenarios.string() << "\n";
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
                  << "  sandbox-path-resolver sandbox-path-resolver/gen_scenarios.py\n"
                  << "  loop-policy           loop-policy/gen_scenarios.py\n"
                  << "  sandbox-virtual-paths sandbox-virtual-paths/gen_scenarios.py\n"
                  << "  glob-matcher          glob-matcher/gen_scenarios.py\n"
                  << "  skill-manager         skill-manager/gen_scenarios.py\n"
                  << "  provider-protocol-support provider-protocol-support/gen_scenarios.py\n"
                  << "  new-chat-persist-dao  new-chat-persist-dao/gen_scenarios.py\n"
                  << "  model-thinking-resolver model-thinking-resolver/gen_scenarios.py\n"
                  << "  shell-tools           shell-tools/gen_scenarios.py\n"
                  << "  attachment-clone-session attachment-clone-session/gen_scenarios.py\n"
                  << "  duckduckgo            duckduckgo/gen_scenarios.py\n"
                  << "  embedding-selector    embedding-selector/gen_scenarios.py\n"
                  << "  migration-16-17       migration-16-17/gen_scenarios.py\n"
                  << "  chat-provider-context-dao chat-provider-context-dao/gen_scenarios.py\n"
                  << "  openai-service-tiers  openai-service-tiers/gen_scenarios.py\n"
                  << "  conch-sse             conch-sse/gen_scenarios.py\n"
                  << "  native-run-archive    native-run-archive/gen_scenarios.py\n"
                  << "  migration-18-19       migration-18-19/gen_scenarios.py\n"
                  << "  streaming-json-document streaming-json-document/gen_scenarios.py\n"
                  << "  message-source        message-source/gen_scenarios.py\n"
                  << "  zip-reader            zip-reader/gen_scenarios.py\n";
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
    } else if (profile == "loop-policy") {
        profile_loop_policy(scenarios);
    } else if (profile == "sandbox-virtual-paths") {
        profile_sandbox_virtual_paths(scenarios);
    } else if (profile == "glob-matcher") {
        profile_glob_matcher(scenarios);
    } else if (profile == "skill-manager") {
        profile_skill_manager(scenarios);
    } else if (profile == "provider-protocol-support") {
        profile_provider_protocol_support(scenarios);
    } else if (profile == "new-chat-persist-dao") {
        profile_new_chat_persist_dao(scenarios);
    } else if (profile == "model-thinking-resolver") {
        profile_model_thinking_resolver(scenarios);
    } else if (profile == "shell-tools") {
        profile_shell_tools(scenarios);
    } else if (profile == "attachment-clone-session") {
        profile_attachment_clone_session(scenarios);
    } else if (profile == "duckduckgo") {
        profile_duckduckgo(scenarios);
    } else if (profile == "embedding-selector") {
        profile_embedding_selector(scenarios);
    } else if (profile == "migration-16-17") {
        profile_migration_16_17(scenarios);
    } else if (profile == "chat-provider-context-dao") {
        profile_chat_provider_context_dao(scenarios);
    } else if (profile == "openai-service-tiers") {
        profile_openai_service_tiers(scenarios);
    } else if (profile == "conch-sse") {
        profile_conch_sse(scenarios);
    } else if (profile == "native-run-archive") {
        profile_native_run_archive(scenarios);
    } else if (profile == "migration-18-19") {
        profile_migration_18_19(scenarios);
    } else if (profile == "streaming-json-document") {
        profile_streaming_json_document(scenarios);
    } else if (profile == "message-source") {
        profile_message_source(scenarios);
    } else if (profile == "zip-reader") {
        profile_zip_reader(scenarios);
    } else {
        std::cerr << "unknown profile: " << profile << "\n";
        return 2;
    }
    return 0;
}