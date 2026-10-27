// Golden parity test for tool/ShellToolBackends.kt + util/SshClient.kt.
//
// In the stand those two files (plus ShellClient.kt, ConchSseParser.kt, ShellToolJson.kt,
// PortableGlobMatcher.kt) compile VERBATIM over scripted doubles: a JSch subset whose host-key
// checks run SshClient's real HostKeyRepository, a scripted streaming HttpClient that seals SSE
// payloads with the session key, the ConchNetwork transport double, and a scripted SandboxManager.
// This side replays every frozen scenario through the C++ port with doubles that mirror those
// exactly — the same event log, the same masked request/stream-call records — and compares the
// outcome to the frozen Kotlin trace byte for byte.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/crypto_primitives.hpp"
#include "agora/core/json.hpp"
#include "agora/core/shell_client.hpp"
#include "agora/core/shell_tool_backends.hpp"
#include "check.hpp"

namespace {

namespace backends = agora::core::shell_backends;
namespace client = agora::core::shell_client;
namespace crypto = agora::core::crypto;
namespace json = agora::core::json;
namespace shell_tools = agora::core::shell_tools;
namespace shell_file = agora::core::shell_file;

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

// ---- JSON builders -------------------------------------------------------------------------

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

std::optional<std::string> optional_present_string(const json::Value& object,
                                                   std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Literal) return std::nullopt;
    return value->text;
}

bool optional_boolean(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) return fallback;
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return fallback;
}

std::optional<bool> optional_boolean_present(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) return std::nullopt;
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return std::nullopt;
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

std::vector<std::string> strings_of(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Array) return {};
    std::vector<std::string> out;
    for (const json::Value& element : value->elements) out.push_back(element.text);
    return out;
}

json::Value optional_result(const std::optional<std::string>& text) {
    if (!text) return jnull();
    return jstr(*text);
}

// ---- The scripted Conch transport (mirrors platform/ConchNetwork.kt) -----------------------

class ScriptedConchTransport final : public client::ConchTransport {
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
            const auto decoded = crypto::base64_decode(kServerPrivatePkcs8, /*url_safe=*/true);
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
        if (at == std::string::npos) return "";
        return url.substr(at + std::string("challenge=").size());
    }

    static crypto::Bytes32 derive_session_key(const client::HeaderList& headers) {
        const std::string* client_pub_header = nullptr;
        for (const auto& header : headers) {
            if (header.first == "X-Client-Public-Key") client_pub_header = &header.second;
        }
        if (client_pub_header == nullptr) error_state("missing X-Client-Public-Key");
        const auto raw = crypto::base64_decode(*client_pub_header, /*url_safe=*/true);
        if (!raw.bytes || raw.bytes->size() != 32) error_state("client key is not 32 bytes");
        crypto::Bytes32 client_public{};
        std::copy(raw.bytes->begin(), raw.bytes->end(), client_public.begin());
        return client::derive_aes_key(server_seed(), client_public);
    }
};

// ---- The scripted streaming transport (mirrors platform/HttpStream.kt) ---------------------

class ScriptedStreamTransport final : public backends::HttpStreamTransport {
public:
    struct Step {
        enum class Kind { Emit, Fail } kind = Kind::Emit;
        std::int32_t code = 200;
        std::optional<std::string> error_body;
        std::vector<std::string> lines;
        client::TransportFailure::Kind failure_kind = client::TransportFailure::Kind::Plain;
        std::string failure_detail;
    };

    struct Call {
        std::string url;
        client::HeaderList headers;
        std::int64_t max_line_bytes = 0;
        std::int64_t max_error_bytes = 0;
    };

    std::vector<Call> calls;
    std::deque<Step> queue;
    bool closed = false;

    void reset() {
        calls.clear();
        queue.clear();
        closed = false;
    }

    std::unique_ptr<backends::HttpStreamHandle> stream_post(
        const backends::StreamCall& call) override;

private:
    static const crypto::Bytes32& server_seed() {
        static const crypto::Bytes32 seed = [] {
            const auto decoded = crypto::base64_decode(kServerPrivatePkcs8, /*url_safe=*/true);
            if (!decoded.bytes) error_state("server key does not decode");
            const auto seed_opt = crypto::pkcs8_x25519_decode(*decoded.bytes);
            if (!seed_opt) error_state("server key is not id-X25519");
            return *seed_opt;
        }();
        return seed;
    }

    [[noreturn]] static void error_state(const char* what) {
        throw agora::core::test::Failure(std::string("stream double: ") + what);
    }

    static std::string seal(const client::HeaderList& headers, const std::string& plaintext) {
        const std::string* client_pub_header = nullptr;
        for (const auto& header : headers) {
            if (header.first == "X-Client-Public-Key") client_pub_header = &header.second;
        }
        if (client_pub_header == nullptr) error_state("missing client key");
        const auto raw = crypto::base64_decode(*client_pub_header, /*url_safe=*/true);
        if (!raw.bytes || raw.bytes->size() != 32) error_state("client key is not 32 bytes");
        crypto::Bytes32 client_public{};
        std::copy(raw.bytes->begin(), raw.bytes->end(), client_public.begin());
        return client::encrypt_payload(client::derive_aes_key(server_seed(), client_public),
                                       plaintext);
    }

    class Handle final : public backends::HttpStreamHandle {
    public:
        Handle(ScriptedStreamTransport& owner, std::int32_t code,
               std::optional<std::string> error_body, std::vector<std::string> lines)
            : owner_(owner),
              code_(code),
              error_body_(std::move(error_body)),
              lines_(std::move(lines)) {}

        std::int32_t code() const override { return code_; }
        std::optional<std::string> error_body() override { return error_body_; }
        std::optional<std::string> read_line() override {
            if (position_ >= lines_.size()) return std::nullopt;
            return lines_[position_++];
        }
        void close() override { owner_.closed = true; }

    private:
        ScriptedStreamTransport& owner_;
        std::int32_t code_;
        std::optional<std::string> error_body_;
        std::vector<std::string> lines_;
        std::size_t position_ = 0;
    };
};

std::unique_ptr<backends::HttpStreamHandle> ScriptedStreamTransport::stream_post(
    const backends::StreamCall& call) {
    calls.push_back(Call{call.url, call.headers, call.max_line_bytes, call.max_error_bytes});
    if (queue.empty()) error_state("http stream script exhausted");
    const Step step = queue.front();
    queue.pop_front();
    if (step.kind == Step::Kind::Fail) {
        throw client::TransportFailure(step.failure_kind, step.failure_detail);
    }
    const bool encrypted =
        std::any_of(call.headers.begin(), call.headers.end(), [](const auto& header) {
            return header.first == "X-Encryption";
        });
    std::vector<std::string> wire_lines;
    for (const std::string& line : step.lines) {
        if (encrypted && line.rfind("data: ", 0) == 0) {
            wire_lines.push_back("data: " + seal(call.headers, line.substr(6)));
        } else {
            wire_lines.push_back(line);
        }
    }
    return std::make_unique<Handle>(*this, step.code, step.error_body, std::move(wire_lines));
}

// ---- The scripted JSch transport (mirrors platform/JSch.kt) --------------------------------

class ScriptedSshTransport final : public backends::SshTransport {
public:
    struct Event {
        enum class Kind {
            SetPassword,
            SetConfig,
            HostKeyCheck,
            Connect,
            OpenChannel,
            SetCommand,
            Disconnect,
            Stat,
            Mkdir,
            Get,
            Put,
            Ls,
            Pwd,
        };
        Kind kind = Kind::Connect;
        std::string text;       // password | key | value | host | channel | kind | command | path | dir
        std::string value;      // config value | presented b64
        std::int32_t number = 0;  // decision | timeout_ms | bytes
        bool flag = false;      // stat found
    };

    // Script state (JschScript).
    std::string presented_key;
    std::string strict_rejection_message = "host key rejected";
    std::optional<std::string> connect_failure;
    std::optional<std::string> open_channel_failure;
    bool never_close = false;
    std::int32_t exit_status = 0;
    std::string stdout_bytes;
    std::string stderr_bytes;
    std::map<std::string, SftpAttrs> stats;
    std::set<std::string> mkdir_failures;
    std::map<std::string, std::string> put_failures;
    std::map<std::string, std::string> file_bytes;
    std::map<std::string, std::string> file_bytes_second;
    std::map<std::string, int> get_counts;
    std::map<std::string, std::vector<LsEntry>> ls_entries;
    std::string pwd_path = "/home/user";

    std::vector<Event> events;

    void reset() {
        presented_key.clear();
        strict_rejection_message = "host key rejected";
        connect_failure.reset();
        open_channel_failure.reset();
        never_close = false;
        exit_status = 0;
        stdout_bytes.clear();
        stderr_bytes.clear();
        stats.clear();
        mkdir_failures.clear();
        put_failures.clear();
        file_bytes.clear();
        file_bytes_second.clear();
        get_counts.clear();
        ls_entries.clear();
        pwd_path = "/home/user";
        events.clear();
        host_.clear();
        user_.clear();
        port_ = 0;
        connected_ = false;
        exec_closed_ = false;
        exec_status_ = 0;
        exec_stdout_.clear();
        exec_stderr_.clear();
        config_.clear();
    }

    // -- session --------------------------------------------------------------------------------

    void open_session(const std::string& user, const std::string& host,
                      std::int32_t port) override {
        user_ = user;
        host_ = host;
        port_ = port;
    }

    void set_password(const std::string& password) override {
        events.push_back(Event{Event::Kind::SetPassword, password, "", 0, false});
    }

    void set_config(const std::string& key, const std::string& value) override {
        config_[key] = value;
        events.push_back(Event{Event::Kind::SetConfig, key, value, 0, false});
    }

    void connect(std::int32_t timeout_ms, backends::HostKeyPolicy& policy) override {
        events.push_back(Event{Event::Kind::Connect, "session", "", timeout_ms, false});
        if (connect_failure) throw std::runtime_error(*connect_failure);
        std::vector<std::uint8_t> key(presented_key.begin(), presented_key.end());
        const int decision = policy.check(host_, key);
        events.push_back(Event{Event::Kind::HostKeyCheck, host_,
                               crypto::base64_encode(key, /*url_safe=*/false, /*padding=*/true),
                               decision, false});
        const bool strict = config_.count("StrictHostKeyChecking") != 0 &&
                            config_["StrictHostKeyChecking"] == "yes";
        if (strict && decision != backends::HostKeyPolicy::Ok) {
            throw std::runtime_error(strict_rejection_message);
        }
        connected_ = true;
    }

    bool is_connected() override { return connected_; }

    void disconnect_session() override {
        connected_ = false;
        events.push_back(Event{Event::Kind::Disconnect, "session", "", 0, false});
    }

    // -- exec channel ---------------------------------------------------------------------------

    void open_exec() override {
        events.push_back(Event{Event::Kind::OpenChannel, "exec", "", 0, false});
        if (open_channel_failure) throw std::runtime_error(*open_channel_failure);
    }

    void set_command(const std::string& command) override {
        events.push_back(Event{Event::Kind::SetCommand, command, "", 0, false});
    }

    void connect_exec(std::int32_t timeout_ms) override {
        events.push_back(Event{Event::Kind::Connect, "exec", "", timeout_ms, false});
        if (connect_failure) throw std::runtime_error(*connect_failure);
        exec_stdout_ = stdout_bytes;
        exec_stderr_ = stderr_bytes;
        exec_status_ = exit_status;
        exec_closed_ = !never_close;
    }

    bool exec_is_closed() override { return exec_closed_; }
    std::int32_t exec_exit_status() override { return exec_status_; }
    std::string exec_stdout() override { return exec_stdout_; }
    std::string exec_stderr() override { return exec_stderr_; }

    void disconnect_exec() override {
        events.push_back(Event{Event::Kind::Disconnect, "exec", "", 0, false});
    }

    // -- sftp channel ---------------------------------------------------------------------------

    void open_sftp() override {
        events.push_back(Event{Event::Kind::OpenChannel, "sftp", "", 0, false});
        if (open_channel_failure) throw std::runtime_error(*open_channel_failure);
    }

    void connect_sftp(std::int32_t timeout_ms) override {
        events.push_back(Event{Event::Kind::Connect, "sftp", "", timeout_ms, false});
        if (connect_failure) throw std::runtime_error(*connect_failure);
    }

    SftpAttrs stat(const std::string& path) override {
        const auto found = stats.find(path);
        events.push_back(Event{Event::Kind::Stat, path, "", 0, found != stats.end()});
        if (found == stats.end()) throw std::runtime_error("No such file: " + path);
        return found->second;
    }

    void mkdir(const std::string& path) override {
        events.push_back(Event{Event::Kind::Mkdir, path, "", 0, false});
        if (mkdir_failures.count(path) != 0) {
            throw std::runtime_error("Permission denied: " + path);
        }
    }

    std::string get(const std::string& path) override {
        events.push_back(Event{Event::Kind::Get, path, "", 0, false});
        const int seen = get_counts[path]++;
        const auto first = file_bytes.find(path);
        if (seen == 0) {
            if (first == file_bytes.end()) throw std::runtime_error("No such file: " + path);
            return first->second;
        }
        const auto second = file_bytes_second.find(path);
        if (second != file_bytes_second.end()) return second->second;
        if (first == file_bytes.end()) throw std::runtime_error("No such file: " + path);
        return first->second;
    }

    void put(const std::string& path, std::string_view bytes) override {
        events.push_back(
            Event{Event::Kind::Put, path, "", static_cast<std::int32_t>(bytes.size()), false});
        const auto failure = put_failures.find(path);
        if (failure != put_failures.end()) throw std::runtime_error(failure->second);
    }

    std::string pwd() override {
        events.push_back(Event{Event::Kind::Pwd, pwd_path, "", 0, false});
        return pwd_path;
    }

    std::vector<LsEntry> ls(const std::string& dir) override {
        events.push_back(Event{Event::Kind::Ls, dir, "", 0, false});
        const auto entries = ls_entries.find(dir);
        if (entries == ls_entries.end()) {
            throw std::runtime_error("No such directory: " + dir);
        }
        return entries->second;
    }

    void disconnect_sftp() override {
        events.push_back(Event{Event::Kind::Disconnect, "sftp", "", 0, false});
    }

    // -- clock ----------------------------------------------------------------------------------

    std::int64_t clock_millis() override { return now_ms_; }
    void sleep_millis(std::int64_t millis) override { now_ms_ += millis; }

private:
    std::string host_;
    std::string user_;
    std::int32_t port_ = 0;
    bool connected_ = false;
    bool exec_closed_ = false;
    std::int32_t exec_status_ = 0;
    std::string exec_stdout_;
    std::string exec_stderr_;
    std::map<std::string, std::string> config_;
    std::int64_t now_ms_ = 0;
};

// ---- The scripted SandboxManager (mirrors platform/ScriptedSandboxManager.kt) ---------------

class ScriptedSandboxManager final : public backends::SandboxManager {
public:
    bool available = true;
    std::optional<SandboxResult> execute_result;
    std::optional<std::string> execute_failure;
    std::optional<agora::core::shell_tools::ShellFileReadResult> file_read_result;
    std::optional<std::string> file_read_failure;
    std::optional<std::string> file_write_error;
    std::optional<std::string> file_write_failure;
    std::optional<agora::core::shell_file::ShellFileEditResult> file_edit_result;
    std::optional<std::string> file_edit_failure;
    std::optional<std::pair<std::vector<std::string>, bool>> file_glob_result;
    std::optional<std::string> file_glob_failure;
    std::optional<backends::GrepOutcome> file_grep_result;
    std::optional<std::string> file_grep_failure;

    std::vector<std::string> calls;

    void clear_script() {
        available = true;
        execute_result.reset();
        execute_failure.reset();
        file_read_result.reset();
        file_read_failure.reset();
        file_write_error.reset();
        file_write_failure.reset();
        file_edit_result.reset();
        file_edit_failure.reset();
        file_glob_result.reset();
        file_glob_failure.reset();
        file_grep_result.reset();
        file_grep_failure.reset();
        calls.clear();
    }

    bool is_available() override {
        calls.push_back("isAvailable");
        return available;
    }

    SandboxResult execute_command(const std::string& /*command*/, const std::string& /*workdir*/,
                                  std::int32_t /*timeout_ms*/) override {
        calls.push_back("executeCommand");
        if (execute_failure) throw std::runtime_error(*execute_failure);
        return execute_result.value_or(SandboxResult{"", "", 0, std::nullopt});
    }

    agora::core::shell_tools::ShellFileReadResult file_read(const std::string& path,
                                                            std::int64_t offset,
                                                            std::int64_t limit) override {
        calls.push_back("fileRead:" + path);
        if (file_read_failure) throw std::runtime_error(*file_read_failure);
        if (file_read_result) return *file_read_result;
        agora::core::shell_tools::ShellFileReadResult result;
        result.offset = std::max<std::int64_t>(offset, 0);
        result.limit = limit;
        return result;
    }

    std::optional<std::string> file_write(const std::string& path,
                                          const std::string& /*content*/) override {
        calls.push_back("fileWrite:" + path);
        if (file_write_failure) throw std::runtime_error(*file_write_failure);
        return file_write_error;
    }

    std::pair<std::vector<std::string>, bool> file_glob(
        const std::string& pattern, const std::string& /*base_path*/,
        const std::optional<std::int32_t>& /*depth*/) override {
        calls.push_back("fileGlob:" + pattern);
        if (file_glob_failure) throw std::runtime_error(*file_glob_failure);
        return file_glob_result.value_or(std::make_pair(std::vector<std::string>{}, false));
    }

    backends::GrepOutcome file_grep(const std::string& pattern, const std::string& /*base_path*/,
                                    const std::string& /*file_glob*/) override {
        calls.push_back("fileGrep:" + pattern);
        if (file_grep_failure) {
            return backends::GrepOutcome{false, {}, false, *file_grep_failure};
        }
        if (file_grep_result) return *file_grep_result;
        return backends::GrepOutcome{true, {}, false, ""};
    }

    agora::core::shell_file::ShellFileEditResult file_edit(const std::string& path,
                                                           const std::string& /*old_string*/,
                                                           const std::string& /*new_string*/,
                                                           bool /*replace_all*/) override {
        calls.push_back("fileEdit:" + path);
        if (file_edit_failure) throw std::runtime_error(*file_edit_failure);
        return file_edit_result.value_or(agora::core::shell_file::ShellFileEditResult{0, "", std::nullopt});
    }
};

// ---- Scenario plumbing ---------------------------------------------------------------------

std::string challenge_from_url(const std::string& url) {
    const std::size_t at = url.find("challenge=");
    if (at == std::string::npos) return "";
    return url.substr(at + std::string("challenge=").size());
}

std::string handshake_json(const std::string& api_key, const std::string& challenge,
                           const std::optional<std::string>& sign_with) {
    const std::string key = sign_with.value_or(api_key);
    const std::string public_key(kServerPublic);
    const std::string features(kRequiredFeatures);
    const std::string nonce = "stand-nonce";
    const std::string security_payload =
        "conch-handshake-v2|" + public_key + "|" + challenge + "|" + features;
    return json::to_kotlin_string(jobj({
        {"public_key", jstr(public_key)},
        {"nonce", jstr(nonce)},
        {"signature", jstr(client::hmac_hex(key, nonce + "|" + public_key))},
        {"features", jstr(features)},
        {"challenge", jstr(challenge)},
        {"features_signature",
         jstr(client::hmac_hex(key, nonce + "|" + security_payload))},
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

backends::ShellDeviceConfig device_of(const json::Value& scenario, const std::string& stem) {
    const json::Value* device = field(scenario, "device");
    if (device == nullptr || device->kind != json::Kind::Object) {
        scenario_fail(stem, "missing device");
    }
    backends::ShellDeviceConfig config;
    config.name = required_string(*device, "name", stem);
    config.description = optional_string_or(*device, "description");
    config.type = optional_string_or(*device, "type", "conch");
    config.server_url = optional_string_or(*device, "serverUrl");
    config.api_key = optional_string_or(*device, "apiKey");
    config.conch_public_key = optional_string_or(*device, "conchPublicKey");
    config.ssh_host = optional_string_or(*device, "sshHost");
    config.ssh_port =
        static_cast<std::int32_t>(optional_integer(*device, "sshPort").value_or(22));
    config.ssh_user = optional_string_or(*device, "sshUser", "root");
    config.ssh_password = optional_string_or(*device, "sshPassword");
    config.ssh_host_key = optional_string_or(*device, "sshHostKey");
    return config;
}

/** installConchFileScript: handshake (when the device has a key) + the endpoint response. */
void install_conch_file_script(const json::Value& scenario, ScriptedConchTransport& transport,
                               const std::string& stem) {
    transport.reset();
    const backends::ShellDeviceConfig device = device_of(scenario, stem);
    transport.handshake_builder = [api_key = device.api_key](const std::string& challenge) {
        return handshake_json(api_key, challenge, std::nullopt);
    };
    if (!device.api_key.empty()) {
        ScriptedConchTransport::Step handshake;
        handshake.dynamic_handshake = true;
        transport.queue.push_back(handshake);
    }
    const json::Value* response = field(scenario, "response");
    if (response == nullptr) return;
    if (response->kind != json::Kind::Object) scenario_fail(stem, "response is not object");
    ScriptedConchTransport::Step step;
    step.code = static_cast<std::int32_t>(optional_integer(*response, "code").value_or(200));
    step.body = optional_string_or(*response, "body");
    step.raw_body = optional_present_string(*response, "raw_body");
    step.signature_mode = optional_string_or(*response, "signature", "absent");
    if (const auto failure = optional_present_string(*response, "failure")) {
        step.kind = ScriptedConchTransport::Step::Kind::Fail;
        step.failure_kind = failure_kind_of(*failure);
        step.failure_detail = optional_string_or(*response, "failure_detail");
    }
    transport.queue.push_back(step);
}

/** installStreamScript: the streaming HttpClient script. */
void install_stream_script(const json::Value& scenario, ScriptedStreamTransport& transport) {
    transport.reset();
    const json::Value* stream = field(scenario, "stream");
    if (stream == nullptr || stream->kind != json::Kind::Object) return;
    ScriptedStreamTransport::Step step;
    if (const auto failure = optional_present_string(*stream, "failure")) {
        step.kind = ScriptedStreamTransport::Step::Kind::Fail;
        step.failure_kind = failure_kind_of(*failure);
        step.failure_detail = optional_string_or(*stream, "failure_detail");
    } else {
        step.code = static_cast<std::int32_t>(optional_integer(*stream, "code").value_or(200));
        step.error_body = optional_present_string(*stream, "error_body");
        step.lines = strings_of(*stream, "lines");
    }
    transport.queue.push_back(step);
}

/** conchExecuteOutcome's script installation order. */
void install_execute_script(const json::Value& scenario, ScriptedConchTransport& conch,
                            ScriptedStreamTransport& stream, const std::string& stem) {
    conch.reset();
    stream.reset();
    const backends::ShellDeviceConfig device = device_of(scenario, stem);
    const std::optional<std::string> sign_with =
        optional_present_string(scenario, "handshake_sign_with");
    conch.handshake_builder = [api_key = device.api_key, sign_with](
                                  const std::string& challenge) {
        return handshake_json(api_key, challenge, sign_with);
    };
    if (!device.api_key.empty() && optional_boolean(scenario, "script_handshake", true)) {
        ScriptedConchTransport::Step handshake;
        handshake.dynamic_handshake = true;
        conch.queue.push_back(handshake);
    }
    install_stream_script(scenario, stream);
}

/** installJschScript. */
void install_jsch_script(const json::Value& scenario, ScriptedSshTransport& ssh) {
    ssh.reset();
    const json::Value* script = field(scenario, "jsch");
    if (script == nullptr || script->kind != json::Kind::Object) return;
    if (const auto value = optional_present_string(*script, "presented_key")) {
        ssh.presented_key = *value;
    }
    if (const auto value = optional_present_string(*script, "strict_rejection")) {
        ssh.strict_rejection_message = *value;
    }
    if (const auto value = optional_present_string(*script, "connect_failure")) {
        ssh.connect_failure = *value;
    }
    if (const auto value = optional_present_string(*script, "open_channel_failure")) {
        ssh.open_channel_failure = *value;
    }
    if (const auto value = optional_integer(*script, "exit_status")) {
        ssh.exit_status = static_cast<std::int32_t>(*value);
    }
    if (const auto value = optional_boolean_present(*script, "never_close")) {
        ssh.never_close = *value;
    }
    if (const auto value = optional_present_string(*script, "stdout")) {
        ssh.stdout_bytes = *value;
    }
    if (const auto value = optional_present_string(*script, "stderr")) {
        ssh.stderr_bytes = *value;
    }
    if (const auto value = optional_present_string(*script, "pwd")) ssh.pwd_path = *value;

    if (const json::Value* stats = field(*script, "stats");
        stats != nullptr && stats->kind == json::Kind::Object) {
        for (const json::Member& member : stats->members) {
            backends::SshTransport::SftpAttrs attrs;
            attrs.is_dir = optional_boolean(member.value, "is_dir", false);
            attrs.size = optional_integer(member.value, "size").value_or(0);
            ssh.stats[member.key] = attrs;
        }
    }
    if (const json::Value* failures = field(*script, "mkdir_failures");
        failures != nullptr && failures->kind == json::Kind::Array) {
        for (const json::Value& element : failures->elements) {
            ssh.mkdir_failures.insert(element.text);
        }
    }
    if (const json::Value* failures = field(*script, "put_failures");
        failures != nullptr && failures->kind == json::Kind::Object) {
        for (const json::Member& member : failures->members) {
            ssh.put_failures[member.key] = member.value.text;
        }
    }
    if (const json::Value* files = field(*script, "file_bytes");
        files != nullptr && files->kind == json::Kind::Object) {
        for (const json::Member& member : files->members) {
            ssh.file_bytes[member.key] = member.value.text;
        }
    }
    if (const json::Value* files = field(*script, "file_bytes_second");
        files != nullptr && files->kind == json::Kind::Object) {
        for (const json::Member& member : files->members) {
            ssh.file_bytes_second[member.key] = member.value.text;
        }
    }
    if (const json::Value* entries = field(*script, "ls");
        entries != nullptr && entries->kind == json::Kind::Object) {
        for (const json::Member& member : entries->members) {
            std::vector<backends::SshTransport::LsEntry> list;
            for (const json::Value& element : member.value.elements) {
                if (element.kind != json::Kind::Array || element.elements.size() < 2) {
                    throw agora::core::test::Failure("jsch ls entry is not a pair");
                }
                list.push_back(backends::SshTransport::LsEntry{
                    element.elements[0].text, element.elements[1].text == "true"});
            }
            ssh.ls_entries[member.key] = std::move(list);
        }
    }
}

/** installSandboxScript. */
void install_sandbox_script(const json::Value& scenario, ScriptedSandboxManager& sandbox) {
    sandbox.clear_script();
    const json::Value* script = field(scenario, "sandbox");
    if (script == nullptr || script->kind != json::Kind::Object) return;
    sandbox.available = optional_boolean(*script, "available", true);
    if (const auto value = optional_present_string(*script, "execute_failure")) {
        sandbox.execute_failure = *value;
    }
    if (const json::Value* result = field(*script, "execute_result");
        result != nullptr && result->kind == json::Kind::Object) {
        sandbox.execute_result = backends::SandboxManager::SandboxResult{
            optional_string_or(*result, "stdout"), optional_string_or(*result, "stderr"),
            static_cast<std::int32_t>(optional_integer(*result, "exit_code").value_or(0)),
            optional_present_string(*result, "warning")};
    }
    if (const auto value = optional_present_string(*script, "file_read_failure")) {
        sandbox.file_read_failure = *value;
    }
    if (const json::Value* result = field(*script, "file_read_result");
        result != nullptr && result->kind == json::Kind::Object) {
        shell_tools::ShellFileReadResult read;
        read.content = optional_string_or(*result, "content");
        read.lines = static_cast<std::int32_t>(optional_integer(*result, "lines").value_or(0));
        read.total_lines =
            static_cast<std::int32_t>(optional_integer(*result, "total_lines").value_or(0));
        read.total_bytes = optional_integer(*result, "total_bytes").value_or(0);
        read.returned_bytes = optional_integer(*result, "returned_bytes").value_or(0);
        read.offset = optional_integer(*result, "offset").value_or(0);
        read.limit = optional_integer(*result, "limit").value_or(0);
        read.truncated = optional_boolean(*result, "truncated", false);
        read.error = optional_present_string(*result, "error");
        sandbox.file_read_result = std::move(read);
    }
    if (const auto value = optional_present_string(*script, "file_write_failure")) {
        sandbox.file_write_failure = *value;
    }
    if (const auto value = optional_present_string(*script, "file_write_error")) {
        sandbox.file_write_error = *value;
    }
    if (const auto value = optional_present_string(*script, "file_edit_failure")) {
        sandbox.file_edit_failure = *value;
    }
    if (const json::Value* result = field(*script, "file_edit_result");
        result != nullptr && result->kind == json::Kind::Object) {
        sandbox.file_edit_result = shell_file::ShellFileEditResult{
            static_cast<int>(optional_integer(*result, "replacements").value_or(0)),
            optional_string_or(*result, "sha256"),
            optional_present_string(*result, "error")};
    }
    if (const auto value = optional_present_string(*script, "file_glob_failure")) {
        sandbox.file_glob_failure = *value;
    }
    if (const json::Value* result = field(*script, "file_glob_result");
        result != nullptr && result->kind == json::Kind::Object) {
        sandbox.file_glob_result = std::make_pair(
            strings_of(*result, "files"), optional_boolean(*result, "truncated", false));
    }
    if (const auto value = optional_present_string(*script, "file_grep_failure")) {
        sandbox.file_grep_failure = *value;
    }
    if (const json::Value* result = field(*script, "file_grep_result");
        result != nullptr && result->kind == json::Kind::Object) {
        backends::GrepOutcome grep;
        grep.ok = true;
        if (const json::Value* matches = field(*result, "matches");
            matches != nullptr && matches->kind == json::Kind::Array) {
            for (const json::Value& element : matches->elements) {
                grep.matches.push_back(backends::GrepMatch{
                    optional_string_or(element, "path"),
                    static_cast<std::int32_t>(optional_integer(element, "line").value_or(0)),
                    optional_string_or(element, "content")});
            }
        }
        grep.truncated = optional_boolean(*result, "truncated", false);
        sandbox.file_grep_result = std::move(grep);
    }
}

// ---- Masked record logs (mirrors the Kotlin driver) ----------------------------------------

std::string mask_header(const std::string& key, const std::string& value) {
    if (key == "X-Timestamp") return "<timestamp>";
    if (key == "X-Signature") return "<signature>";
    if (key == "X-Nonce") return "<nonce>";
    if (key == "X-Client-Public-Key") return "<client-public-key>";
    return value;
}

std::string mask_url(const std::string& url) {
    const std::size_t at = url.find("challenge=");
    if (at == std::string::npos) return url;
    std::size_t end = url.find('&', at);
    if (end == std::string::npos) end = url.size();
    return url.substr(0, at) + "challenge=<challenge>" + url.substr(end);
}

json::Value conch_requests_json(const ScriptedConchTransport& transport) {
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
             request.call_timeout_millis
                 ? jnum(std::to_string(*request.call_timeout_millis))
                 : jnull()},
        }));
    }
    return jarr(std::move(entries));
}

json::Value stream_calls_json(const ScriptedStreamTransport& transport) {
    std::vector<json::Value> entries;
    for (const auto& call : transport.calls) {
        std::vector<json::Value> headers;
        for (const auto& header : call.headers) {
            headers.push_back(jarr({jstr(header.first),
                                    jstr(mask_header(header.first, header.second))}));
        }
        entries.push_back(jobj({
            {"url", jstr(call.url)},
            {"headers", jarr(std::move(headers))},
            {"max_line_bytes", jnum(std::to_string(call.max_line_bytes))},
            {"max_error_bytes", jnum(std::to_string(call.max_error_bytes))},
        }));
    }
    return jarr(std::move(entries));
}

json::Value ssh_events_json(const ScriptedSshTransport& transport) {
    std::vector<json::Value> entries;
    for (const auto& event : transport.events) {
        using Kind = ScriptedSshTransport::Event::Kind;
        switch (event.kind) {
            case Kind::SetConfig:
                entries.push_back(jobj({{"event", jstr("set_config")},
                                        {"key", jstr(event.text)},
                                        {"value", jstr(event.value)}}));
                break;
            case Kind::SetPassword:
                entries.push_back(
                    jobj({{"event", jstr("set_password")}, {"password", jstr(event.text)}}));
                break;
            case Kind::HostKeyCheck:
                entries.push_back(jobj({{"event", jstr("host_key_check")},
                                        {"host", jstr(event.text)},
                                        {"decision", jnum(std::to_string(event.number))}}));
                break;
            case Kind::Connect:
                entries.push_back(jobj({{"event", jstr("connect")},
                                        {"channel", jstr(event.text)},
                                        {"timeout_ms", jnum(std::to_string(event.number))}}));
                break;
            case Kind::OpenChannel:
                entries.push_back(
                    jobj({{"event", jstr("open_channel")}, {"kind", jstr(event.text)}}));
                break;
            case Kind::SetCommand:
                entries.push_back(
                    jobj({{"event", jstr("set_command")}, {"command", jstr(event.text)}}));
                break;
            case Kind::Disconnect:
                entries.push_back(
                    jobj({{"event", jstr("disconnect")}, {"channel", jstr(event.text)}}));
                break;
            case Kind::Stat:
                entries.push_back(jobj({{"event", jstr("stat")},
                                        {"path", jstr(event.text)},
                                        {"found", jbool(event.flag)}}));
                break;
            case Kind::Mkdir:
                entries.push_back(
                    jobj({{"event", jstr("mkdir")}, {"path", jstr(event.text)}}));
                break;
            case Kind::Get:
                entries.push_back(jobj({{"event", jstr("get")}, {"path", jstr(event.text)}}));
                break;
            case Kind::Put:
                entries.push_back(jobj({{"event", jstr("put")},
                                        {"path", jstr(event.text)},
                                        {"bytes", jnum(std::to_string(event.number))}}));
                break;
            case Kind::Ls:
                entries.push_back(jobj({{"event", jstr("ls")}, {"dir", jstr(event.text)}}));
                break;
            case Kind::Pwd:
                entries.push_back(jobj({{"event", jstr("pwd")}, {"path", jstr(event.text)}}));
                break;
        }
    }
    return jarr(std::move(entries));
}

json::Value sandbox_calls_json(const ScriptedSandboxManager& sandbox) {
    std::vector<json::Value> entries;
    for (const std::string& call : sandbox.calls) entries.push_back(jstr(call));
    return jarr(std::move(entries));
}

// ---- Outcome fragments --------------------------------------------------------------------

json::Value file_read_result_json(const shell_tools::ShellFileReadResult& result) {
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
}

json::Value file_edit_result_json(const shell_file::ShellFileEditResult& result) {
    return jobj({
        {"replacements", jnum(std::to_string(result.replacements))},
        {"sha256", jstr(result.sha256)},
        {"error", optional_result(result.error)},
    });
}

json::Value glob_result_json(const backends::GlobOutcome& outcome) {
    std::vector<json::Value> files;
    for (const std::string& file : outcome.files) files.push_back(jstr(file));
    return jobj({
        {"files", jarr(std::move(files))},
        {"truncated", jbool(outcome.truncated)},
    });
}

json::Value grep_result_json(const backends::GrepOutcome& outcome) {
    std::vector<json::Value> matches;
    for (const backends::GrepMatch& match : outcome.matches) {
        matches.push_back(jobj({
            {"path", jstr(match.path)},
            {"line", jnum(std::to_string(match.line))},
            {"content", jstr(match.content)},
        }));
    }
    return jobj({
        {"matches", jarr(std::move(matches))},
        {"truncated", jbool(outcome.truncated)},
    });
}

/** The Result.fold outcome shape shared by every glob/grep op. */
json::Value folded_outcome(bool ok, const json::Value& result, const std::string& error) {
    if (ok) return jobj({{"result", result}, {"threw", jnull()}});
    return jobj({{"result", jnull()}, {"threw", jstr(error)}});
}

// ---- Conch ops ----------------------------------------------------------------------------

json::Value conch_execute_outcome(const json::Value& scenario, const std::string& stem,
                                  bool events) {
    ScriptedConchTransport conch;
    ScriptedStreamTransport stream;
    install_execute_script(scenario, conch, stream, stem);
    const backends::ShellDeviceConfig device = device_of(scenario, stem);
    backends::ConchBackend backend(device, conch, stream);
    const std::string command = required_string(scenario, "command", stem);
    const std::string workdir = optional_string_or(scenario, "workdir");
    const auto timeout_ms =
        static_cast<std::int32_t>(optional_integer(scenario, "timeout_ms").value_or(0));

    if (events) {
        std::vector<json::Value> event_list;
        std::optional<std::string> failure;
        try {
            backend.execute_command_events(
                command, workdir, timeout_ms, [&](const backends::BackendEvent& event) {
                    if (event.kind == backends::BackendEvent::Kind::OutputDelta) {
                        event_list.push_back(jobj(
                            {{"type", jstr("delta")}, {"text", jstr(event.text)}}));
                    } else {
                        event_list.push_back(jobj({{"type", jstr("completed")},
                                                   {"text", jstr(event.text)},
                                                   {"is_error", jbool(event.is_error)}}));
                    }
                });
        } catch (const std::exception& error) {
            failure = error.what();
        }
        return jobj({
            {"events", failure ? jnull() : jarr(std::move(event_list))},
            {"threw", failure ? jstr(*failure) : jnull()},
            {"stream_calls", stream_calls_json(stream)},
            {"requests", conch_requests_json(conch)},
            {"closed", jbool(stream.closed)},
        });
    }

    std::optional<std::string> result;
    std::optional<std::string> threw;
    try {
        result = backend.execute_command(command, workdir, timeout_ms);
    } catch (const std::exception& error) {
        threw = error.what();
    }
    return jobj({
        {"result", optional_result(result)},
        {"threw", optional_result(threw)},
        {"stream_calls", stream_calls_json(stream)},
        {"requests", conch_requests_json(conch)},
        {"closed", jbool(stream.closed)},
    });
}

json::Value conch_file_read(const json::Value& scenario, const std::string& stem) {
    ScriptedConchTransport conch;
    ScriptedStreamTransport stream;
    install_conch_file_script(scenario, conch, stem);
    backends::ConchBackend backend(device_of(scenario, stem), conch, stream);
    std::optional<shell_tools::ShellFileReadResult> result;
    std::optional<std::string> threw;
    try {
        result = backend.file_read(
            required_string(scenario, "path", stem),
            optional_integer(scenario, "offset").value_or(0),
            optional_integer(scenario, "limit").value_or(0));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    if (threw) {
        return jobj({{"result", jnull()},
                     {"threw", jstr(*threw)},
                     {"requests", conch_requests_json(conch)}});
    }
    return jobj({{"result", file_read_result_json(*result)},
                 {"threw", jnull()},
                 {"requests", conch_requests_json(conch)}});
}

json::Value conch_file_write(const json::Value& scenario, const std::string& stem) {
    ScriptedConchTransport conch;
    ScriptedStreamTransport stream;
    install_conch_file_script(scenario, conch, stem);
    backends::ConchBackend backend(device_of(scenario, stem), conch, stream);
    std::optional<std::string> result;
    std::optional<std::string> threw;
    try {
        result = backend.file_write(required_string(scenario, "path", stem),
                                    required_string(scenario, "content", stem));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    return jobj({{"result", threw ? jnull() : optional_result(result)},
                 {"threw", optional_result(threw)},
                 {"requests", conch_requests_json(conch)}});
}

json::Value conch_file_edit(const json::Value& scenario, const std::string& stem) {
    ScriptedConchTransport conch;
    ScriptedStreamTransport stream;
    install_conch_file_script(scenario, conch, stem);
    backends::ConchBackend backend(device_of(scenario, stem), conch, stream);
    std::optional<shell_file::ShellFileEditResult> result;
    std::optional<std::string> threw;
    try {
        result = backend.file_edit(
            required_string(scenario, "path", stem),
            required_string(scenario, "old_string", stem),
            required_string(scenario, "new_string", stem),
            optional_boolean(scenario, "replace_all", false));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    return jobj({{"result", threw ? jnull() : file_edit_result_json(*result)},
                 {"threw", optional_result(threw)},
                 {"requests", conch_requests_json(conch)}});
}

json::Value conch_file_glob(const json::Value& scenario, const std::string& stem) {
    ScriptedConchTransport conch;
    ScriptedStreamTransport stream;
    install_conch_file_script(scenario, conch, stem);
    backends::ConchBackend backend(device_of(scenario, stem), conch, stream);
    std::optional<backends::GlobOutcome> outcome;
    std::optional<std::string> threw;
    try {
        const auto depth = optional_integer(scenario, "depth");
        outcome = backend.file_glob(
            required_string(scenario, "pattern", stem),
            optional_string_or(scenario, "path"),
            depth ? std::optional<std::int32_t>(static_cast<std::int32_t>(*depth))
                  : std::nullopt);
    } catch (const std::exception& error) {
        threw = error.what();
    }
    json::Value folded = threw
                             ? folded_outcome(false, jnull(), *threw)
                             : folded_outcome(outcome->ok,
                                              outcome->ok ? glob_result_json(*outcome) : jnull(),
                                              outcome->error);
    folded.members.push_back(json::Member{"requests", conch_requests_json(conch)});
    return folded;
}

json::Value conch_file_grep(const json::Value& scenario, const std::string& stem) {
    ScriptedConchTransport conch;
    ScriptedStreamTransport stream;
    install_conch_file_script(scenario, conch, stem);
    backends::ConchBackend backend(device_of(scenario, stem), conch, stream);
    std::optional<backends::GrepOutcome> outcome;
    std::optional<std::string> threw;
    try {
        outcome = backend.file_grep(required_string(scenario, "pattern", stem),
                                    optional_string_or(scenario, "path"),
                                    optional_string_or(scenario, "glob"));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    json::Value folded = threw
                             ? folded_outcome(false, jnull(), *threw)
                             : folded_outcome(outcome->ok,
                                              outcome->ok ? grep_result_json(*outcome) : jnull(),
                                              outcome->error);
    folded.members.push_back(json::Member{"requests", conch_requests_json(conch)});
    return folded;
}

// ---- SSH ops ------------------------------------------------------------------------------

std::optional<std::string> captured_host_key(const ScriptedSshTransport& ssh) {
    std::optional<std::string> captured;
    for (const auto& event : ssh.events) {
        if (event.kind == ScriptedSshTransport::Event::Kind::HostKeyCheck) {
            captured = event.value;
        }
    }
    return captured;
}

json::Value ssh_execute_outcome(const json::Value& scenario, const std::string& stem,
                                bool events) {
    ScriptedSshTransport ssh;
    install_jsch_script(scenario, ssh);
    backends::SshBackend backend(device_of(scenario, stem), ssh);
    const std::string command = required_string(scenario, "command", stem);
    const std::string workdir = optional_string_or(scenario, "workdir");
    const auto timeout_ms =
        static_cast<std::int32_t>(optional_integer(scenario, "timeout_ms").value_or(0));

    if (events) {
        std::vector<json::Value> event_list;
        std::optional<std::string> failure;
        try {
            backend.execute_command_events(
                command, workdir, timeout_ms, [&](const backends::BackendEvent& event) {
                    if (event.kind == backends::BackendEvent::Kind::Completed) {
                        event_list.push_back(jobj({{"type", jstr("completed")},
                                                   {"text", jstr(event.text)},
                                                   {"is_error", jbool(event.is_error)}}));
                    } else {
                        event_list.push_back(jobj(
                            {{"type", jstr("other")}, {"text", jstr(event.text)}}));
                    }
                });
        } catch (const std::exception& error) {
            failure = error.what();
        }
        backend.close();
        return jobj({
            {"events", failure ? jnull() : jarr(std::move(event_list))},
            {"threw", optional_result(failure)},
            {"ssh_events", ssh_events_json(ssh)},
        });
    }

    std::optional<std::string> result;
    std::optional<std::string> threw;
    try {
        result = backend.execute_command(command, workdir, timeout_ms);
    } catch (const std::exception& error) {
        threw = error.what();
    }
    backend.close();
    const std::optional<std::string> captured = captured_host_key(ssh);
    return jobj({
        {"result", optional_result(result)},
        {"threw", optional_result(threw)},
        {"captured_host_key", captured ? jstr(*captured) : jnull()},
        {"ssh_events", ssh_events_json(ssh)},
    });
}

json::Value ssh_fingerprint_outcome(const json::Value& scenario, const std::string& stem) {
    const std::string fingerprint =
        backends::SshClient::fingerprint_sha256(required_string(scenario, "base64_key", stem));
    return jobj({{"result", jstr(fingerprint)}});
}

json::Value ssh_file_read(const json::Value& scenario, const std::string& stem) {
    ScriptedSshTransport ssh;
    install_jsch_script(scenario, ssh);
    backends::SshBackend backend(device_of(scenario, stem), ssh);
    std::optional<shell_tools::ShellFileReadResult> result;
    std::optional<std::string> threw;
    try {
        result = backend.file_read(
            required_string(scenario, "path", stem),
            optional_integer(scenario, "offset").value_or(0),
            optional_integer(scenario, "limit").value_or(0));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    backend.close();
    return jobj({
        {"result", result ? file_read_result_json(*result) : jnull()},
        {"threw", optional_result(threw)},
        {"ssh_events", ssh_events_json(ssh)},
    });
}

json::Value ssh_file_write(const json::Value& scenario, const std::string& stem) {
    ScriptedSshTransport ssh;
    install_jsch_script(scenario, ssh);
    backends::SshBackend backend(device_of(scenario, stem), ssh);
    std::optional<std::string> result;
    std::optional<std::string> threw;
    try {
        result = backend.file_write(required_string(scenario, "path", stem),
                                    required_string(scenario, "content", stem));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    backend.close();
    return jobj({
        {"result", threw ? jnull() : optional_result(result)},
        {"threw", optional_result(threw)},
        {"ssh_events", ssh_events_json(ssh)},
    });
}

json::Value ssh_file_edit(const json::Value& scenario, const std::string& stem) {
    ScriptedSshTransport ssh;
    install_jsch_script(scenario, ssh);
    backends::SshBackend backend(device_of(scenario, stem), ssh);
    std::optional<shell_file::ShellFileEditResult> result;
    std::optional<std::string> threw;
    try {
        result = backend.file_edit(
            required_string(scenario, "path", stem),
            required_string(scenario, "old_string", stem),
            required_string(scenario, "new_string", stem),
            optional_boolean(scenario, "replace_all", false));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    backend.close();
    return jobj({
        {"result", result ? file_edit_result_json(*result) : jnull()},
        {"threw", optional_result(threw)},
        {"ssh_events", ssh_events_json(ssh)},
    });
}

json::Value ssh_file_glob(const json::Value& scenario, const std::string& stem) {
    ScriptedSshTransport ssh;
    install_jsch_script(scenario, ssh);
    backends::SshBackend backend(device_of(scenario, stem), ssh);
    std::optional<backends::GlobOutcome> outcome;
    std::optional<std::string> threw;
    try {
        const auto depth = optional_integer(scenario, "depth");
        outcome = backend.file_glob(
            required_string(scenario, "pattern", stem),
            optional_string_or(scenario, "path"),
            depth ? std::optional<std::int32_t>(static_cast<std::int32_t>(*depth))
                  : std::nullopt);
    } catch (const std::exception& error) {
        threw = error.what();
    }
    backend.close();
    json::Value folded = threw
                             ? folded_outcome(false, jnull(), *threw)
                             : folded_outcome(outcome->ok,
                                              outcome->ok ? glob_result_json(*outcome) : jnull(),
                                              outcome->error);
    folded.members.push_back(json::Member{"ssh_events", ssh_events_json(ssh)});
    return folded;
}

json::Value ssh_file_grep(const json::Value& scenario, const std::string& stem) {
    ScriptedSshTransport ssh;
    install_jsch_script(scenario, ssh);
    backends::SshBackend backend(device_of(scenario, stem), ssh);
    std::optional<backends::GrepOutcome> outcome;
    std::optional<std::string> threw;
    try {
        outcome = backend.file_grep(required_string(scenario, "pattern", stem),
                                    optional_string_or(scenario, "path"),
                                    optional_string_or(scenario, "glob"));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    backend.close();
    json::Value folded = threw
                             ? folded_outcome(false, jnull(), *threw)
                             : folded_outcome(outcome->ok,
                                              outcome->ok ? grep_result_json(*outcome) : jnull(),
                                              outcome->error);
    folded.members.push_back(json::Member{"ssh_events", ssh_events_json(ssh)});
    return folded;
}

// ---- Sandbox ops --------------------------------------------------------------------------

json::Value sandbox_execute_outcome(const json::Value& scenario, const std::string& stem) {
    ScriptedSandboxManager sandbox;
    install_sandbox_script(scenario, sandbox);
    backends::SandboxBackend backend(&sandbox);
    std::optional<std::string> result;
    std::optional<std::string> threw;
    try {
        result = backend.execute_command(
            required_string(scenario, "command", stem),
            optional_string_or(scenario, "workdir"),
            static_cast<std::int32_t>(optional_integer(scenario, "timeout_ms").value_or(0)));
    } catch (const std::exception& error) {
        threw = error.what();
    }
    return jobj({
        {"result", optional_result(result)},
        {"threw", optional_result(threw)},
        {"sandbox_calls", sandbox_calls_json(sandbox)},
    });
}

json::Value sandbox_file_read(const json::Value& scenario, const std::string& stem) {
    ScriptedSandboxManager sandbox;
    install_sandbox_script(scenario, sandbox);
    backends::SandboxBackend backend(&sandbox);
    const shell_tools::ShellFileReadResult result = backend.file_read(
        required_string(scenario, "path", stem),
        optional_integer(scenario, "offset").value_or(0),
        optional_integer(scenario, "limit").value_or(0));
    return jobj({
        {"result", file_read_result_json(result)},
        {"sandbox_calls", sandbox_calls_json(sandbox)},
    });
}

json::Value sandbox_file_write(const json::Value& scenario, const std::string& stem) {
    ScriptedSandboxManager sandbox;
    install_sandbox_script(scenario, sandbox);
    backends::SandboxBackend backend(&sandbox);
    const std::optional<std::string> result = backend.file_write(
        required_string(scenario, "path", stem), required_string(scenario, "content", stem));
    return jobj({
        {"result", optional_result(result)},
        {"sandbox_calls", sandbox_calls_json(sandbox)},
    });
}

json::Value sandbox_file_edit(const json::Value& scenario, const std::string& stem) {
    ScriptedSandboxManager sandbox;
    install_sandbox_script(scenario, sandbox);
    backends::SandboxBackend backend(&sandbox);
    const shell_file::ShellFileEditResult result = backend.file_edit(
        required_string(scenario, "path", stem),
        required_string(scenario, "old_string", stem),
        required_string(scenario, "new_string", stem),
        optional_boolean(scenario, "replace_all", false));
    return jobj({
        {"result", file_edit_result_json(result)},
        {"sandbox_calls", sandbox_calls_json(sandbox)},
    });
}

json::Value sandbox_file_glob(const json::Value& scenario, const std::string& stem) {
    ScriptedSandboxManager sandbox;
    install_sandbox_script(scenario, sandbox);
    backends::SandboxBackend backend(&sandbox);
    const auto depth = optional_integer(scenario, "depth");
    const backends::GlobOutcome outcome = backend.file_glob(
        required_string(scenario, "pattern", stem), optional_string_or(scenario, "path"),
        depth ? std::optional<std::int32_t>(static_cast<std::int32_t>(*depth))
              : std::nullopt);
    json::Value folded = folded_outcome(
        outcome.ok, outcome.ok ? glob_result_json(outcome) : jnull(), outcome.error);
    folded.members.push_back(json::Member{"sandbox_calls", sandbox_calls_json(sandbox)});
    return folded;
}

json::Value sandbox_file_grep(const json::Value& scenario, const std::string& stem) {
    ScriptedSandboxManager sandbox;
    install_sandbox_script(scenario, sandbox);
    backends::SandboxBackend backend(&sandbox);
    const backends::GrepOutcome outcome =
        backend.file_grep(required_string(scenario, "pattern", stem),
                          optional_string_or(scenario, "path"),
                          optional_string_or(scenario, "glob"));
    json::Value folded = folded_outcome(
        outcome.ok, outcome.ok ? grep_result_json(outcome) : jnull(), outcome.error);
    folded.members.push_back(json::Member{"sandbox_calls", sandbox_calls_json(sandbox)});
    return folded;
}

json::Value run_op(const json::Value& scenario, const std::string& op,
                   const std::string& stem) {
    if (op == "conch_execute") return conch_execute_outcome(scenario, stem, false);
    if (op == "conch_execute_events") return conch_execute_outcome(scenario, stem, true);
    if (op == "conch_file_read") return conch_file_read(scenario, stem);
    if (op == "conch_file_write") return conch_file_write(scenario, stem);
    if (op == "conch_file_edit") return conch_file_edit(scenario, stem);
    if (op == "conch_file_glob") return conch_file_glob(scenario, stem);
    if (op == "conch_file_grep") return conch_file_grep(scenario, stem);
    if (op == "ssh_execute") return ssh_execute_outcome(scenario, stem, false);
    if (op == "ssh_execute_events") return ssh_execute_outcome(scenario, stem, true);
    if (op == "ssh_fingerprint") return ssh_fingerprint_outcome(scenario, stem);
    if (op == "ssh_file_read") return ssh_file_read(scenario, stem);
    if (op == "ssh_file_write") return ssh_file_write(scenario, stem);
    if (op == "ssh_file_edit") return ssh_file_edit(scenario, stem);
    if (op == "ssh_file_glob") return ssh_file_glob(scenario, stem);
    if (op == "ssh_file_grep") return ssh_file_grep(scenario, stem);
    if (op == "sandbox_execute") return sandbox_execute_outcome(scenario, stem);
    if (op == "sandbox_file_read") return sandbox_file_read(scenario, stem);
    if (op == "sandbox_file_write") return sandbox_file_write(scenario, stem);
    if (op == "sandbox_file_edit") return sandbox_file_edit(scenario, stem);
    if (op == "sandbox_file_glob") return sandbox_file_glob(scenario, stem);
    if (op == "sandbox_file_grep") return sandbox_file_grep(scenario, stem);
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

TEST(shell_tool_backends_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_TOOL_BACKENDS_DIR;
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
