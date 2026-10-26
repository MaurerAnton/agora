// Golden parity test for the transport-neutral policy in api/HttpClient.kt.
//
// Replays every scenario under core/tools/golden/http-policy/scenarios through the C++ port and
// requires the produced trace to equal the frozen Kotlin trace byte for byte. The Kotlin side
// extracts the pure declarations verbatim from HttpClient.kt and exercises them on the JVM; this
// side drives the same policy without OkHttp, sockets or Android diagnostics.
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

#include "agora/core/http_policy.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace http = agora::core::http_policy;
namespace json = agora::core::json;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

// ---- JSON value construction -------------------------------------------------------------------

json::Value string_value(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value literal_value(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value number_value(std::int64_t number) { return literal_value(std::to_string(number)); }

json::Value bool_value(bool value) { return literal_value(value ? "true" : "false"); }

json::Value null_value() { return literal_value("null"); }

json::Value array_value(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value object_value(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

// ---- scenario parsing --------------------------------------------------------------------------

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

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::String) return std::nullopt;
    return value->text;
}

std::optional<std::int64_t> optional_long(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) return std::nullopt;
    return std::stoll(value->text);
}

std::int64_t required_long(const json::Value& object, std::string_view name,
                           const std::string& scenario) {
    const std::optional<std::int64_t> value = optional_long(object, name);
    if (!value) scenario_fail(scenario, "missing number " + std::string(name));
    return *value;
}

bool required_bool(const json::Value& object, std::string_view name, const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal ||
        (value->text != "true" && value->text != "false")) {
        scenario_fail(scenario, "missing boolean " + std::string(name));
    }
    return value->text == "true";
}

std::vector<std::pair<std::string, std::string>> pairs(const json::Value& object,
                                                       std::string_view name) {
    std::vector<std::pair<std::string, std::string>> out;
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Array) return out;
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::Array || element.elements.size() != 2) continue;
        out.emplace_back(element.elements[0].text, element.elements[1].text);
    }
    return out;
}

std::vector<std::string> string_list(const json::Value& object, std::string_view name) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Array) return out;
    for (const json::Value& element : value->elements) {
        if (element.kind == json::Kind::String) out.push_back(element.text);
    }
    return out;
}

std::optional<http::ProxyConfig> parse_config(const json::Value& scenario,
                                              const std::string& stem) {
    const json::Value* value = field(scenario, "config");
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::Literal) {
        if (value->text != "null") scenario_fail(stem, "config is neither null nor an object");
        return std::nullopt;
    }
    if (value->kind != json::Kind::Object) scenario_fail(stem, "config is not an object");

    const std::string type = required_string(*value, "type", stem);
    http::ProxyConfig config;
    if (type == "HTTP") {
        config.type = http::ProxyType::kHttp;
    } else if (type == "SOCKS") {
        config.type = http::ProxyType::kSocks;
    } else {
        scenario_fail(stem, "unknown proxy type " + type);
    }
    config.host = required_string(*value, "host", stem);
    config.port = static_cast<int>(required_long(*value, "port", stem));
    config.username = optional_string(*value, "username").value_or("");
    config.password = optional_string(*value, "password").value_or("");
    config.bypass = string_list(*value, "bypass");
    return config;
}

// ---- trace construction ------------------------------------------------------------------------

json::Value strings_value(const std::vector<std::string>& values) {
    std::vector<json::Value> elements;
    elements.reserve(values.size());
    for (const std::string& value : values) elements.push_back(string_value(value));
    return array_value(std::move(elements));
}

json::Value headers_value(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::vector<json::Value> elements;
    elements.reserve(headers.size());
    for (const auto& [name, value] : headers) {
        elements.push_back(array_value({string_value(name), string_value(value)}));
    }
    return array_value(std::move(elements));
}

json::Value config_value(const std::optional<http::ProxyConfig>& config) {
    if (!config) return null_value();
    return object_value({
        {"type", string_value(config->type == http::ProxyType::kHttp ? "HTTP" : "SOCKS")},
        {"host", string_value(config->host)},
        {"port", number_value(config->port)},
        {"username", string_value(config->username)},
        {"password", string_value(config->password)},
        {"bypass", strings_value(config->bypass)},
    });
}

json::Value proxy_value(const http::ProxyDecision& proxy) {
    std::vector<std::pair<std::string, json::Value>> members;
    const char* type = proxy.kind == http::ProxyKind::kDirect
                           ? "DIRECT"
                           : (proxy.kind == http::ProxyKind::kHttp ? "HTTP" : "SOCKS");
    members.emplace_back("type", string_value(type));
    if (proxy.kind != http::ProxyKind::kDirect) {
        members.emplace_back("host", string_value(proxy.host));
        members.emplace_back("port", number_value(proxy.port));
    }
    return object_value(std::move(members));
}

json::Value policy_value(const http::ClientPolicy& policy) {
    return object_value({
        {"connectTimeoutSeconds", number_value(policy.connect_timeout_seconds)},
        {"readTimeoutSeconds", number_value(policy.read_timeout_seconds)},
        {"writeTimeoutSeconds", number_value(policy.write_timeout_seconds)},
        {"retryOnConnectionFailure", bool_value(policy.retry_on_connection_failure)},
        {"followRedirects", bool_value(policy.follow_redirects)},
        {"followSslRedirects", bool_value(policy.follow_ssl_redirects)},
    });
}

json::Value error_value(const std::string& type, const std::string& message) {
    return object_value({
        {"error", object_value({{"type", string_value(type)},
                                 {"message", string_value(message)}})},
    });
}

// ---- replay ------------------------------------------------------------------------------------

std::string run_scenario(const json::Value& scenario, const std::string& op,
                         const std::string& stem) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("op", string_value(op));
    json::Value outcome;

    if (op == "isLocalHost") {
        const std::string host = required_string(scenario, "host", stem);
        members.emplace_back("host", string_value(host));
        outcome = object_value({{"local", bool_value(http::is_local_host(host))}});
    } else if (op == "guardCleartextCredentials") {
        const std::string url = required_string(scenario, "url", stem);
        const auto headers = pairs(scenario, "headers");
        members.emplace_back("url", string_value(url));
        members.emplace_back("headers", headers_value(headers));
        try {
            http::guard_cleartext_credentials(url, headers);
            outcome = object_value({{"ok", bool_value(true)}});
        } catch (const std::runtime_error& error) {
            outcome = error_value("IOException", error.what());
        }
    } else if (op == "sanitizeProxy") {
        const std::optional<http::ProxyConfig> config = parse_config(scenario, stem);
        members.emplace_back("config", config_value(config));
        outcome =
            object_value({{"config", config_value(http::sanitize_proxy_config(config))}});
    } else if (op == "resolveProxy") {
        const std::string host = required_string(scenario, "host", stem);
        const std::optional<http::ProxyConfig> config = parse_config(scenario, stem);
        members.emplace_back("host", string_value(host));
        members.emplace_back("config", config_value(config));
        outcome = object_value({{"proxy",
                                 proxy_value(http::resolve_proxy(
                                     host, http::sanitize_proxy_config(config)))}});
    } else if (op == "isProxyBypassed") {
        const std::string host = required_string(scenario, "host", stem);
        const std::vector<std::string> bypass = string_list(scenario, "bypass");
        members.emplace_back("host", string_value(host));
        members.emplace_back("bypass", strings_value(bypass));
        outcome =
            object_value({{"bypassed", bool_value(http::is_proxy_bypassed(host, bypass))}});
    } else if (op == "ipv4InCidr") {
        const std::string host = required_string(scenario, "host", stem);
        const std::string cidr = required_string(scenario, "cidr", stem);
        members.emplace_back("host", string_value(host));
        members.emplace_back("cidr", string_value(cidr));
        outcome = object_value({{"match", bool_value(http::ipv4_in_cidr(host, cidr))}});
    } else if (op == "proxyAuthorization") {
        const std::optional<http::ProxyConfig> config = parse_config(scenario, stem);
        const bool existing = required_bool(scenario, "hasExistingHeader", stem);
        members.emplace_back("config", config_value(config));
        members.emplace_back("hasExistingHeader", bool_value(existing));
        const std::optional<std::string> header = http::proxy_authorization_header(
            http::sanitize_proxy_config(config), existing);
        outcome = object_value({{"header", header ? string_value(*header) : null_value()}});
    } else if (op == "clientPolicy") {
        outcome = policy_value(http::client_policy());
    } else {
        scenario_fail(stem, "unknown op: " + op);
    }

    members.emplace_back("outcome", std::move(outcome));
    return json::to_kotlin_string(object_value(std::move(members))) + "\n";
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

TEST(http_policy_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_HTTP_POLICY_DIR;
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
    for (const std::filesystem::path& scenario_path : scenarios) {
        const std::string stem = scenario_path.stem().string();
        const json::ParseResult parsed = json::parse(read_file(scenario_path));
        if (!parsed.value || parsed.value->kind != json::Kind::Object) {
            scenario_fail(stem, "scenario is not a JSON object");
        }
        const std::string op = required_string(*parsed.value, "op", stem);
        const std::string actual = run_scenario(*parsed.value, op, stem);
        const std::string expected = read_file(golden_dir / (stem + ".trace"));
        CHECK(!expected.empty());
        if (actual != expected) {
            throw agora::core::test::Failure(stem + ": " + describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
