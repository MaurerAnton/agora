// Golden parity test for ShellClient.kt (the Conch HTTP client).
//
// In the stand, ShellClient.kt + ShellCrypto.kt + ConchHandshake.kt compile VERBATIM over the
// platform transport double; this side replays every frozen scenario through the C++ port with
// a scripted transport that mirrors the double exactly — recorded requests (challenge/header/
// ciphertext masked the same way), dynamic handshake construction, and response sealing with
// the very composition the client verifies (X25519 symmetry via the fixed stand server key).
// The outcome is compared to the frozen Kotlin trace byte for byte.
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/json.hpp"
#include "agora/core/shell_client.hpp"
#include "check.hpp"

namespace {

namespace client = agora::core::shell_client;
namespace crypto = agora::core::crypto;
namespace json = agora::core::json;

constexpr std::string_view kServerPublic = "ChlxST1g8D4sd0DzZmQdk76qjaeoeyJxGaY7a-g1W3k";
constexpr std::string_view kServerPrivatePkcs8 =
    "MC4CAQAwBQYDK2VuBCIEIHFn0CpncaiXmIxD1seMg1VuVjEsnCQC7We5bp39n6wL";
constexpr std::string_view kRequiredFeatures =
    "hkdf-extract-v2,response-auth-v1,sse-sequence-v1,rejection-auth-v1";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

// ---- JSON builders -------------------------------------------------------------------------------

json::Value jstr(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value jnum(std::string digits) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(digits);
    return value;
}

json::Value jbool(bool flag) { return jnum(flag ? "true" : "false"); }

json::Value jnull() { return jnum("null"); }

json::Value jobj(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

json::Value jarr(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

std::string required_string(const json::Value& object, std::string_view name,
                            const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr ||
        (value->kind != json::Kind::String && value->kind != json::Kind::Literal)) {
        scenario_fail(scenario, "missing string " + std::string(name));
    }
    return value->text;
}

std::string optional_string_or(const json::Value& object, std::string_view name,
                               std::string fallback = "") {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Literal) return fallback;
    return value->text;
}

bool optional_boolean(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) return fallback;
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return fallback;
}

std::optional<std::int64_t> optional_integer(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal || value->text == "null") {
        return std::nullopt;
    }
    try {
        return std::stoll(value->text);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> optional_present_string(const json::Value& object,
                                                   std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Literal) return std::nullopt;
    return value->text;
}

json::Value optional_result(std::optional<std::string> text) {
    if (!text) return jnull();
    return jstr(*text);
}

// ---- The scripted transport (mirrors platform/ConchNetwork.kt) ------------------------------------

class ScriptedTransport final : public client::ConchTransport {
public:
    struct Step {
        enum class Kind { Emit, Fail } kind = Kind::Emit;
        std::int32_t code = 200;
        std::string body;
        std::optional<std::string> raw_body;
        std::string signature_mode = "absent";
        bool dynamic_handshake = false;
        client::TransportFailure::Kind failure_kind = client::TransportFailure::Kind::Plain;
        std::string failure_detail;
    };

    struct Request {
        std::string url;
        std::string method;
        std::string body;
        client::HeaderList headers;
        std::optional<std::int64_t> call_timeout_millis;
    };

    std::vector<Request> requests;
    std::deque<Step> queue;
    std::function<std::string(const std::string&)> handshake_builder;

    void reset() {
        requests.clear();
        queue.clear();
        handshake_builder = nullptr;
    }

    client::ConchTextResponse get_text_response(const std::string& url,
                                                const client::HeaderList& headers) override {
        requests.push_back(Request{url, "GET", "", headers, std::nullopt});
        return respond();
    }

    client::ConchTextResponse post_text_response(const std::string& url, const std::string& body,
                                                 const client::HeaderList& headers,
                                                 const std::optional<std::int64_t>&
                                                     call_timeout_millis) override {
        requests.push_back(Request{url, "POST", body, headers, call_timeout_millis});
        return respond();
    }

private:
    static const crypto::Bytes32& server_seed() {
        static const crypto::Bytes32 seed = [] {
            const auto decoded =
                crypto::base64_decode(kServerPrivatePkcs8, /*url_safe=*/true);
            if (!decoded.bytes) error_state("server key does not decode");
            const auto seed_opt = crypto::pkcs8_x25519_decode(*decoded.bytes);
            if (!seed_opt) error_state("server key is not id-X25519");
            return *seed_opt;
        }();
        return seed;
    }

    [[noreturn]] static void error_state(const char* what) {
        throw agora::core::test::Failure(std::string("transport double: ") + what);
    }

    client::ConchTextResponse respond() {
        if (queue.empty()) error_state("conch script exhausted");
        const Step step = queue.front();
        queue.pop_front();
        if (step.kind == Step::Kind::Fail) {
            throw client::TransportFailure(step.failure_kind, step.failure_detail);
        }

        const Request& request = requests.back();
        const client::HeaderList& headers = request.headers;
        const bool encrypted =
            std::any_of(headers.begin(), headers.end(), [](const auto& header) {
                return header.first == "X-Encryption";
            });
        const bool seal = encrypted && step.code >= 200 && step.code <= 299;

        std::string wire_body = step.body;
        std::optional<crypto::Bytes32> session_key;
        if (seal) {
            session_key = derive_session_key(headers);
            wire_body = client::encrypt_payload(*session_key, step.body);
        }
        if (step.raw_body) wire_body = *step.raw_body;

        std::string signature;
        if (step.signature_mode == "invalid") {
            signature = std::string(64, '0');
        } else if (seal) {
            signature = client::response_signature(*session_key, step.code, wire_body);
        } else if (step.signature_mode == "valid") {
            signature = client::response_signature(derive_session_key(headers), step.code,
                                                   wire_body);
        }

        if (step.dynamic_handshake) {
            if (!handshake_builder) error_state("handshake builder not installed");
            wire_body = handshake_builder(challenge_of(request.url));
        }
        return client::ConchTextResponse{step.code, wire_body,
                                         step.code >= 200 && step.code <= 299, signature};
    }

    static std::string challenge_of(const std::string& url) {
        const std::size_t at = url.find("challenge=");
        if (at == std::string::npos) return url;
        std::size_t end = url.find('&', at);
        if (end == std::string::npos) end = url.size();
        return url.substr(at + std::string("challenge=").size(), end - (at + 10));
    }

    static crypto::Bytes32 derive_session_key(const client::HeaderList& headers) {
        const auto* client_pub_header = [&]() -> const std::string* {
            for (const auto& header : headers) {
                if (header.first == "X-Client-Public-Key") return &header.second;
            }
            return nullptr;
        }();
        if (client_pub_header == nullptr) error_state("missing X-Client-Public-Key");
        const auto raw = crypto::base64_decode(*client_pub_header, /*url_safe=*/true);
        if (!raw.bytes || raw.bytes->size() != 32) error_state("client key is not 32 bytes");
        crypto::Bytes32 client_public{};
        std::copy(raw.bytes->begin(), raw.bytes->end(), client_public.begin());
        return client::derive_aes_key(server_seed(), client_public);
    }
};

// ---- Scenario plumbing ----------------------------------------------------------------------------

std::string challenge_from_url(const std::string& url) {
    const std::size_t at = url.find("challenge=");
    if (at == std::string::npos) return url;
    std::size_t end = url.find('&', at);
    if (end == std::string::npos) end = url.size();
    return url.substr(at + 10, end - (at + 10));
}

std::string handshake_json(const json::Value& scenario, const std::string& challenge,
                           const std::string& scenario_name) {
    const std::string sign_with =
        optional_string_or(scenario, "handshake_sign_with",
                           required_string(scenario, "api_key", scenario_name));
    const std::string features =
        optional_string_or(scenario, "handshake_features", std::string(kRequiredFeatures));
    const std::string challenge_field =
        optional_string_or(scenario, "handshake_challenge", challenge);
    const std::string public_key =
        optional_string_or(scenario, "handshake_public_key", std::string(kServerPublic));
    const std::string nonce = "stand-nonce";
    const std::string security_payload = "conch-handshake-v2|" + public_key + "|" +
                                         challenge_field + "|" + features;
    return json::to_kotlin_string(jobj({
        {"public_key", jstr(public_key)},
        {"nonce", jstr(nonce)},
        {"signature", jstr(client::hmac_hex(sign_with, nonce + "|" + public_key))},
        {"features", jstr(features)},
        {"challenge", jstr(challenge_field)},
        {"features_signature",
         jstr(client::hmac_hex(sign_with, nonce + "|" + security_payload))},
    }));
}

client::TransportFailure::Kind failure_kind_of(const std::string& tag) {
    using Kind = client::TransportFailure::Kind;
    if (tag == "unknown_host") return Kind::UnknownHost;
    if (tag == "connect") return Kind::Connect;
    if (tag == "socket_timeout") return Kind::SocketTimeout;
    if (tag == "interrupted_io") return Kind::InterruptedIO;
    if (tag == "ssl") return Kind::Ssl;
    return Kind::Plain;
}

void install_handshake_builder(const json::Value& scenario, ScriptedTransport& transport,
                               const std::string& stem) {
    transport.handshake_builder = [&](const std::string& challenge) {
        return handshake_json(scenario, challenge, stem);
    };
}

/** installResponses: handshake GET first, then the endpoint response/failure (driver order). */
void install_responses(const json::Value& scenario, ScriptedTransport& transport,
                       const std::string& stem) {
    const std::string api_key = required_string(scenario, "api_key", stem);
    if (!api_key.empty() && optional_boolean(scenario, "script_handshake", true)) {
        ScriptedTransport::Step handshake;
        handshake.dynamic_handshake = true;
        transport.queue.push_back(handshake);
    }
    if (const json::Value* response = field(scenario, "response")) {
        if (response->kind != json::Kind::Object) scenario_fail(stem, "response is not object");
        ScriptedTransport::Step step;
        step.code = static_cast<std::int32_t>(optional_integer(*response, "code").value_or(200));
        step.body = optional_string_or(*response, "body");
        step.raw_body = optional_present_string(*response, "raw_body");
        step.signature_mode = optional_string_or(*response, "signature", "absent");
        transport.queue.push_back(step);
    }
    if (const json::Value* failure = field(scenario, "transport_failure")) {
        if (failure->kind != json::Kind::String) {
            scenario_fail(stem, "transport_failure is not a string");
        }
        ScriptedTransport::Step step;
        step.kind = ScriptedTransport::Step::Kind::Fail;
        step.failure_kind = failure_kind_of(failure->text);
        step.failure_detail = optional_string_or(scenario, "transport_failure_detail");
        transport.queue.push_back(step);
    }
}

/** The fresh client the driver builds for every op (reset, builder, construct). */
client::ShellClient make_client(const json::Value& scenario, ScriptedTransport& transport,
                                const std::string& stem) {
    transport.reset();
    install_handshake_builder(scenario, transport, stem);
    return client::ShellClient(
        required_string(scenario, "server_url", stem),
        required_string(scenario, "api_key", stem),
        optional_string_or(scenario, "cached_public_key"), transport);
}

// ---- Request masking (mirror of the Kotlin driver) ------------------------------------------------

std::string mask_url(const std::string& url) {
    const std::size_t at = url.find("challenge=");
    if (at == std::string::npos) return url;
    std::size_t end = url.find('&', at);
    if (end == std::string::npos) end = url.size();
    return url.substr(0, at) + "challenge=<challenge>" + url.substr(end);
}

std::string mask_header(const std::string& key, const std::string& value) {
    if (key == "X-Timestamp") return "<timestamp>";
    if (key == "X-Signature") return "<signature>";
    if (key == "X-Nonce") return "<nonce>";
    if (key == "X-Client-Public-Key") return "<client-public-key>";
    return value;
}

json::Value requests_json(const ScriptedTransport& transport) {
    std::vector<json::Value> entries;
    for (const auto& request : transport.requests) {
        const bool encrypted =
            std::any_of(request.headers.begin(), request.headers.end(), [](const auto& header) {
                return header.first == "X-Encryption";
            });
        std::vector<json::Value> headers;
        for (const auto& header : request.headers) {
            headers.push_back(jarr({jstr(header.first),
                                    jstr(mask_header(header.first, header.second))}));
        }
        entries.push_back(jobj({
            {"url", jstr(mask_url(request.url))},
            {"method", jstr(request.method)},
            {"body", jstr(encrypted ? "<encrypted>" : request.body)},
            {"headers", jarr(std::move(headers))},
            {"call_timeout_ms",
             request.call_timeout_millis ? jnum(std::to_string(*request.call_timeout_millis))
                                         : jnull()},
        }));
    }
    return jarr(std::move(entries));
}

// ---- Op outcome helpers --------------------------------------------------------------------------

json::Value with_requests(std::optional<json::Value> result, std::optional<std::string> threw,
                          const ScriptedTransport& transport) {
    return jobj({
        {"result", result ? *result : jnull()},
        {"threw", threw ? jstr(*threw) : jnull()},
        {"requests", requests_json(transport)},
    });
}

/** callWithRequests: run the client call, mapping success/failure to the driver's shape. */
template <typename Fn>
json::Value call_with_requests(ScriptedTransport& transport, Fn&& block) {
    try {
        std::optional<json::Value> result = block();
        return with_requests(result, std::nullopt, transport);
    } catch (const std::exception& failure) {
        return with_requests(std::nullopt, std::string(failure.what()), transport);
    }
}

// ---- Ops -----------------------------------------------------------------------------------------

json::Value fetch_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    transport.reset();
    install_handshake_builder(scenario, transport, stem);
    const bool http_failure = optional_boolean(scenario, "handshake_http_failure", false);
    if (const json::Value* transport_failure = field(scenario, "transport_failure")) {
        if (transport_failure->kind != json::Kind::String) {
            scenario_fail(stem, "transport_failure is not a string");
        }
        ScriptedTransport::Step step;
        step.kind = ScriptedTransport::Step::Kind::Fail;
        step.failure_kind = failure_kind_of(transport_failure->text);
        step.failure_detail = optional_string_or(scenario, "transport_failure_detail");
        transport.queue.push_back(step);
    } else if (http_failure) {
        ScriptedTransport::Step step;
        step.code = static_cast<std::int32_t>(
            optional_integer(scenario, "handshake_failure_code").value_or(503));
        step.body = optional_string_or(scenario, "handshake_failure_body");
        transport.queue.push_back(step);
    } else {
        ScriptedTransport::Step step;
        step.dynamic_handshake = true;
        transport.queue.push_back(step);
    }
    client::ShellClient client(
        required_string(scenario, "server_url", stem),
        required_string(scenario, "api_key", stem),
        optional_string_or(scenario, "cached_public_key"), transport);
    const bool result = client.fetch_public_key();
    return jobj({
        {"result", jbool(result)},
        {"last_error", optional_result(client.last_error())},
        {"server_public_key", optional_result(client.get_server_public_key_base64())},
        {"requests", requests_json(transport)},
    });
}

json::Value prepare_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    if (optional_boolean(scenario, "run_fetch_first", false)) {
        client.fetch_public_key();
    }
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        const auto request = client.prepare_request(
            required_string(scenario, "command", stem),
            static_cast<std::int32_t>(optional_integer(scenario, "timeout_ms").value_or(0)),
            required_string(scenario, "workdir", stem));
        std::vector<json::Value> headers;
        for (const auto& header : request.headers) {
            headers.push_back(
                jarr({jstr(header.first), jstr(mask_header(header.first, header.second))}));
        }
        return jobj({
            {"body", jstr(request.is_encrypted ? "<encrypted>" : request.body)},
            {"headers", jarr(std::move(headers))},
            {"is_encrypted", jbool(request.is_encrypted)},
            {"server_url", jstr(request.server_url)},
        });
    });
}

json::Value decrypt_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    if (optional_boolean(scenario, "run_fetch_first", false)) {
        client.fetch_public_key();
    }
    if (optional_boolean(scenario, "prepare_first", false)) {
        client.prepare_request(required_string(scenario, "command", stem),
                               static_cast<std::int32_t>(
                                   optional_integer(scenario, "timeout_ms").value_or(0)),
                               "");
    }
    std::string data;
    if (optional_boolean(scenario, "seal_for_session", false)) {
        const auto key = client.get_session_key();
        if (!key) scenario_fail(stem, "no session key to seal with");
        data = client::encrypt_payload(
            *key, required_string(scenario, "plaintext", stem));
    } else {
        data = required_string(scenario, "data", stem);
    }
    return call_with_requests(transport,
                              [&]() -> std::optional<json::Value> {
                                  return jstr(client.decrypt_sse_data(data));
                              });
}

json::Value file_read_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        const auto result = client.file_read(
            required_string(scenario, "path", stem),
            optional_integer(scenario, "offset").value_or(0),
            optional_integer(scenario, "limit").value_or(0));
        return jobj({
            {"content", jstr(result.content)},
            {"lines", jnum(std::to_string(result.lines))},
            {"total_lines", jnum(std::to_string(result.total_lines))},
            {"total_bytes", jnum(std::to_string(result.total_bytes))},
            {"returned_bytes", jnum(std::to_string(result.returned_bytes))},
            {"offset", jnum(std::to_string(result.offset))},
            {"limit", jnum(std::to_string(result.limit))},
            {"truncated", jbool(result.truncated)},
            {"error", optional_result(result.error)},
        });
    });
}

json::Value file_write_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(
        transport, [&]() -> std::optional<json::Value> {
            return optional_result(client.file_write(
                required_string(scenario, "path", stem),
                required_string(scenario, "content", stem)));
        });
}

json::Value file_edit_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        const auto result = client.file_edit(
            required_string(scenario, "path", stem),
            required_string(scenario, "old_string", stem),
            required_string(scenario, "new_string", stem),
            optional_boolean(scenario, "replace_all", false));
        return jobj({
            {"replacements", jnum(std::to_string(result.replacements))},
            {"sha256", jstr(result.sha256)},
            {"error", optional_result(result.error)},
        });
    });
}

json::Value file_glob_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        const auto depth = optional_integer(scenario, "depth");
        const auto result = client.file_glob(
            required_string(scenario, "pattern", stem),
            optional_string_or(scenario, "path"),
            depth ? std::optional<std::int32_t>(static_cast<std::int32_t>(*depth))
                  : std::nullopt);
        if (!result.ok) throw client::ClientError(result.error);
        std::vector<json::Value> files;
        for (const std::string& file : result.files) files.push_back(jstr(file));
        return jobj({
            {"files", jarr(std::move(files))},
            {"truncated", jbool(result.truncated)},
        });
    });
}

json::Value file_grep_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        const auto result = client.file_grep(
            required_string(scenario, "pattern", stem),
            optional_string_or(scenario, "path"),
            optional_string_or(scenario, "glob"));
        if (!result.ok) throw client::ClientError(result.error);
        std::vector<json::Value> matches;
        for (const auto& match : result.matches) {
            matches.push_back(jobj({
                {"path", jstr(match.path)},
                {"line", jnum(std::to_string(match.line))},
                {"content", jstr(match.content)},
            }));
        }
        return jobj({
            {"matches", jarr(std::move(matches))},
            {"truncated", jbool(result.truncated)},
        });
    });
}

json::Value file_image_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        const auto result = client.file_image(required_string(scenario, "path", stem));
        return jobj({
            {"data", jstr(result.data)},
            {"mimeType", jstr(result.mime_type)},
            {"size", jnum(std::to_string(result.size))},
            {"error", optional_result(result.error)},
        });
    });
}

json::Value job_outcome(const json::Value& scenario, const std::string& op,
                        const std::string& stem) {
    ScriptedTransport transport;
    client::ShellClient client = make_client(scenario, transport, stem);
    install_responses(scenario, transport, stem);
    return call_with_requests(transport, [&]() -> std::optional<json::Value> {
        std::string payload;
        if (op == "job_start") {
            payload = client.start_job(
                required_string(scenario, "command", stem),
                static_cast<std::int32_t>(optional_integer(scenario, "timeout_ms").value_or(0)),
                required_string(scenario, "workdir", stem));
        } else if (op == "job_list") {
            payload = client.list_jobs();
        } else if (op == "job_get") {
            payload = client.get_job(required_string(scenario, "job_id", stem));
        } else if (op == "job_stop") {
            payload = client.stop_job(required_string(scenario, "job_id", stem),
                                      optional_integer(scenario, "call_timeout_ms"));
        } else {
            payload = client.acknowledge_job(required_string(scenario, "job_id", stem));
        }
        return jstr(payload);
    });
}

json::Value run_op(const json::Value& scenario, const std::string& op, const std::string& stem) {
    if (op == "fetch_public_key") return fetch_outcome(scenario, stem);
    if (op == "prepare_request") return prepare_outcome(scenario, stem);
    if (op == "decrypt_sse") return decrypt_outcome(scenario, stem);
    if (op == "file_read") return file_read_outcome(scenario, stem);
    if (op == "file_write") return file_write_outcome(scenario, stem);
    if (op == "file_edit") return file_edit_outcome(scenario, stem);
    if (op == "file_glob") return file_glob_outcome(scenario, stem);
    if (op == "file_grep") return file_grep_outcome(scenario, stem);
    if (op == "file_image") return file_image_outcome(scenario, stem);
    if (op == "job_start" || op == "job_list" || op == "job_get" || op == "job_stop" ||
        op == "job_ack") {
        return job_outcome(scenario, op, stem);
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

TEST(shell_client_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_CLIENT_DIR;
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
        const json::Value outcome = run_op(*parsed.value, op, stem);
        const json::Value trace = jobj({{"op", jstr(op)},
                                        {"scenario", *parsed.value},
                                        {"outcome", outcome}});
        const std::string actual = json::to_kotlin_string(trace) + "\n";
        const std::string expected = read_file(golden_dir / (stem + ".trace"));
        CHECK(!expected.empty());
        if (actual != expected) {
            throw agora::core::test::Failure(stem + ": " + describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
