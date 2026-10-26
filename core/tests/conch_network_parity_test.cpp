// Golden parity test for ConchNetwork.kt and BoundedWireRead.kt.
//
// Replays every scenario under core/tools/golden/conch-network/scenarios through the C++ port and
// requires the produced trace to equal the frozen Kotlin trace byte for byte. The Kotlin side
// compiles both app sources verbatim over stand-local OkHttp/okio doubles; this side drives the
// same request/response policy through the Transport/WireSource boundaries the app adapter fills.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/bounded_wire_read.hpp"
#include "agora/core/conch_network.hpp"
#include "agora/core/json.hpp"
#include "agora/core/sha256.hpp"
#include "check.hpp"

namespace {

namespace conch = agora::core::conch;
namespace json = agora::core::json;
namespace wire = agora::core::wire;

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

json::Value number_value(std::int64_t number) {
    return literal_value(std::to_string(number));
}

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

std::string sha256_hex(std::string_view bytes) {
    const auto digest = agora::core::sha256(bytes);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
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

std::vector<conch::Header> conch_headers(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::vector<conch::Header> headers;
    headers.reserve(pairs.size());
    for (const auto& [name, value] : pairs) {
        headers.push_back(conch::Header{name, value});
    }
    return headers;
}

// ---- scripted wire source ----------------------------------------------------------------------

class ScriptedSource final : public wire::Source {
public:
    ScriptedSource(std::string bytes, std::int64_t declared_size, bool* closed)
        : bytes_(std::move(bytes)), declared_size_(declared_size), closed_(closed) {}

    ~ScriptedSource() override {
        if (closed_ != nullptr) *closed_ = true;
    }

    bool request(std::int64_t byte_count) override {
        return declared_size_ - position_ >= byte_count;
    }

    bool exhausted() override { return declared_size_ - position_ <= 0; }

    std::string read_utf8() override {
        const std::size_t start =
            position_ < static_cast<std::int64_t>(bytes_.size())
                ? static_cast<std::size_t>(position_)
                : bytes_.size();
        position_ = declared_size_;
        return bytes_.substr(start);
    }

    std::string read_utf8_line_strict(std::int64_t limit) override {
        const std::size_t start =
            position_ < static_cast<std::int64_t>(bytes_.size())
                ? static_cast<std::size_t>(position_)
                : bytes_.size();
        const std::size_t newline = bytes_.find('\n', start);
        if (newline == std::string::npos ||
            static_cast<std::int64_t>(newline - start) > limit) {
            throw wire::Eof{};
        }
        std::size_t end = newline;
        if (end > start && bytes_[end - 1] == '\r') --end;
        position_ = static_cast<std::int64_t>(newline + 1);
        return bytes_.substr(start, end - start);
    }

    std::int64_t buffer_size() override { return declared_size_ - position_; }

private:
    std::string bytes_;
    std::int64_t declared_size_;
    std::int64_t position_ = 0;
    bool* closed_;
};

struct ScriptedResponse {
    int code = 200;
    std::string body;
    std::int64_t declared_size = 0;
    std::vector<conch::Header> headers;
};

class ScriptedTransport final : public conch::Transport {
public:
    ScriptedResponse response;
    std::optional<conch::Request> last_request;
    bool closed = false;

    conch::RawResponse execute(const conch::Request& request) override {
        last_request = request;
        conch::RawResponse raw;
        raw.code = response.code;
        raw.headers = response.headers;
        raw.body = std::make_unique<ScriptedSource>(response.body, response.declared_size, &closed);
        return raw;
    }
};

// ---- trace construction ------------------------------------------------------------------------

json::Value error_value(const std::string& type, const std::string& message) {
    return object_value({
        {"error", object_value({{"type", string_value(type)},
                                 {"message", string_value(message)}})},
    });
}

json::Value body_value(const std::string& body) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"bodyLength", number_value(static_cast<std::int64_t>(body.size()))},
        {"bodySha256", string_value(sha256_hex(body))},
    };
    if (json::utf16_length(body) <= 64) members.push_back({"bodyText", string_value(body)});
    return object_value(std::move(members));
}

json::Value response_value(const conch::TextResponse& response) {
    std::vector<std::pair<std::string, json::Value>> members{
        {"code", number_value(response.code)},
        {"successful", bool_value(response.is_successful)},
        {"signature", string_value(response.signature)},
        {"bodyLength", number_value(static_cast<std::int64_t>(response.body.size()))},
        {"bodySha256", string_value(sha256_hex(response.body))},
    };
    if (json::utf16_length(response.body) <= 64) {
        members.push_back({"bodyText", string_value(response.body)});
    }
    return object_value(std::move(members));
}

json::Value request_value(const conch::Request& request) {
    std::vector<json::Value> headers;
    headers.reserve(request.headers.size());
    for (const conch::Header& header : request.headers) {
        headers.push_back(array_value({string_value(header.name), string_value(header.value)}));
    }
    return object_value({
        {"method", string_value(request.method)},
        {"url", string_value(request.url)},
        {"headers", array_value(std::move(headers))},
        {"body", request.body ? string_value(*request.body) : null_value()},
        {"contentType", request.content_type.empty() ? null_value()
                                                     : string_value(request.content_type)},
        {"timeoutMillis", number_value(request.timeout_millis)},
    });
}

json::Value config_value() {
    const conch::ClientConfig config = conch::client_config();
    return object_value({
        {"retryOnConnectionFailure", bool_value(config.retry_on_connection_failure)},
        {"followRedirects", bool_value(config.follow_redirects)},
        {"followSslRedirects", bool_value(config.follow_ssl_redirects)},
        {"poolMaxIdle", number_value(config.pool_max_idle_connections)},
        {"poolKeepAliveSeconds", number_value(config.pool_keep_alive_seconds)},
    });
}

std::string run_conch_scenario(const json::Value& scenario, const std::string& op,
                               const std::string& stem) {
    const json::Value* response_object = field(scenario, "response");
    if (response_object == nullptr || response_object->kind != json::Kind::Object) {
        scenario_fail(stem, "missing response");
    }

    ScriptedTransport transport;
    transport.response.code =
        static_cast<int>(optional_long(*response_object, "code").value_or(200));
    transport.response.body = required_string(*response_object, "body", stem);
    transport.response.declared_size =
        optional_long(*response_object, "declaredSize")
            .value_or(static_cast<std::int64_t>(transport.response.body.size()));
    transport.response.headers = conch_headers(pairs(*response_object, "headers"));

    const std::vector<conch::Header> headers = conch_headers(pairs(scenario, "headers"));
    const std::string url = required_string(scenario, "url", stem);

    std::optional<conch::TextResponse> result;
    std::string error_type;
    std::string error_message;
    try {
        if (op == "getTextResponse") {
            result = conch::get_text_response(transport, url, headers);
        } else {
            result = conch::post_text_response(
                transport, url, required_string(scenario, "body", stem), headers,
                optional_long(scenario, "timeoutMillis"));
        }
    } catch (const std::invalid_argument& error) {
        error_type = "IllegalArgumentException";
        error_message = error.what();
    } catch (const std::runtime_error& error) {
        error_type = "IOException";
        error_message = error.what();
    }

    return json::to_kotlin_string(object_value({
               {"op", string_value(op)},
               {"config", config_value()},
               {"request", transport.last_request ? request_value(*transport.last_request)
                                                   : null_value()},
               {"outcome", result ? response_value(*result)
                                   : error_value(error_type, error_message)},
               {"closed", bool_value(transport.closed)},
           })) +
           "\n";
}

std::string run_read_scenario(const json::Value& scenario, const std::string& op,
                              const std::string& stem) {
    const std::int64_t limit = required_long(scenario, "limit", stem);
    const std::string body = required_string(scenario, "body", stem);
    const std::int64_t declared_size =
        optional_long(scenario, "declaredSize")
            .value_or(static_cast<std::int64_t>(body.size()));
    ScriptedSource source(body, declared_size, nullptr);

    json::Value outcome;
    try {
        if (op == "readBoundedWireText") {
            outcome = body_value(wire::read_bounded_wire_text(source, limit));
        } else {
            const std::optional<std::string> line = wire::read_bounded_wire_line(source, limit);
            outcome = object_value({{"line", line ? string_value(*line) : null_value()}});
        }
    } catch (const std::invalid_argument& error) {
        outcome = error_value("IllegalArgumentException", error.what());
    } catch (const std::runtime_error& error) {
        outcome = error_value("IOException", error.what());
    }

    return json::to_kotlin_string(object_value({
               {"op", string_value(op)},
               {"limit", number_value(limit)},
               {"source", object_value({
                              {"bodyLength", number_value(static_cast<std::int64_t>(body.size()))},
                              {"declaredSize", number_value(declared_size)},
                          })},
               {"outcome", std::move(outcome)},
           })) +
           "\n";
}

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;
    const std::string op = required_string(scenario, "op", stem);
    if (op == "getTextResponse" || op == "postTextResponse") {
        return run_conch_scenario(scenario, op, stem);
    }
    if (op == "readBoundedWireText" || op == "readBoundedWireLine") {
        return run_read_scenario(scenario, op, stem);
    }
    scenario_fail(stem, "unknown op: " + op);
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

TEST(conch_network_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CONCH_NETWORK_DIR;
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
