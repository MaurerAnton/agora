// Golden parity test for the C++ ConchHandshake port.
//
// Replays every scenario under core/tools/golden/conch-handshake/scenarios through
// agora::core::conch::Handshake and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/conch-handshake/ConchHandshakeTraceDump.kt): the handshake object is
// re-serialized compactly exactly like kotlinx JsonObject.toString() before it is handed to
// ConchHandshake.decode, and the output is {"valid":true|false}.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/conch_handshake.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace conch = agora::core::conch;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
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

std::string required_string(const json::Value& object, std::string_view name,
                            const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::String) {
        scenario_fail(scenario, "missing string " + std::string(name));
    }
    return value->text;
}

// kotlinx JsonObject.toString(): compact (no whitespace), member order as parsed, with the
// standard JSON escapes (" \ and the short control forms).
std::string kotlinx_compact(const json::Value& value, const std::string& scenario) {
    switch (value.kind) {
        case json::Kind::Object: {
            std::string out = "{";
            bool first = true;
            for (const json::Member& member : value.members) {
                if (!first) out += ',';
                first = false;
                out += '"';
                for (char c : member.key) {
                    switch (c) {
                        case '"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b"; break;
                        case '\t': out += "\\t"; break;
                        case '\n': out += "\\n"; break;
                        case '\f': out += "\\f"; break;
                        case '\r': out += "\\r"; break;
                        default:
                            if (static_cast<unsigned char>(c) < 0x20) {
                                std::ostringstream hex;
                                hex << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                                    << static_cast<int>(static_cast<unsigned char>(c));
                                out += hex.str();
                            } else {
                                out += c;
                            }
                    }
                }
                out += '"';
                out += ':';
                out += kotlinx_compact(member.value, scenario);            }
            return out + "}";
        }
        case json::Kind::String: {
            std::string out = "\"";
            for (char c : value.text) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\t': out += "\\t"; break;
                    case '\n': out += "\\n"; break;
                    case '\f': out += "\\f"; break;
                    case '\r': out += "\\r"; break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            std::ostringstream hex;
                            hex << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                                << static_cast<int>(static_cast<unsigned char>(c));
                            out += hex.str();
                        } else {
                            out += c;
                        }
                }
            }
            return out + '"';
        }
        case json::Kind::Array: {
            std::string out = "[";
            bool first = true;
            for (const json::Value& element : value.elements) {
                if (!first) out += ',';
                first = false;
                out += kotlinx_compact(element, scenario);
            }
            return out + "]";
        }
        case json::Kind::Literal:
            return value.text;
    }
    scenario_fail(scenario, "unreachable");
}

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string op = required_string(scenario, "op", stem);
    if (op != "verify") scenario_fail(stem, "unknown op: " + op);

    const std::string api_key = required_string(scenario, "api_key", stem);
    const std::string expected_challenge = required_string(scenario, "expected_challenge", stem);
    const json::Value* handshake = field(scenario, "handshake");
    if (handshake == nullptr || handshake->kind != json::Kind::Object) {
        scenario_fail(stem, "missing handshake object");
    }

    const conch::Handshake decoded = conch::Handshake::decode(kotlinx_compact(*handshake, stem));
    const bool valid = decoded.verify(api_key, expected_challenge);
    return std::string("{\"valid\":") + (valid ? "true" : "false") + "}\n";
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(160, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(conch_handshake_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CONCH_HANDSHAKE_DIR;
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
        checked++;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
