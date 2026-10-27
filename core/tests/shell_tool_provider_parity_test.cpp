// Golden parity test for tool/ShellToolProvider.kt.
//
// In the stand ShellToolProvider.kt compiles VERBATIM over the already-pinned leaf modules and
// over the real ShellToolBackends.kt / ShellToolDefinitions.kt / ShellDurableJobExecutor.kt,
// with scripted doubles for the platform seams (ConchNetwork, the streaming HttpClient, JSch,
// SandboxManager, SandboxManagerFactory, ToolImageStore and the minimal GenerationContext).
// This side replays every frozen scenario through the C++ port with doubles that mirror those
// exactly — the same request/event/sandbox/image logs, the same confirmation calls and the same
// masked real-clock values — and compares the rendered trace to the Kotlin one byte for byte.
#include <algorithm>
#include <cctype>
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
#include "agora/core/shell_tool_provider.hpp"
#include "check.hpp"

namespace {

namespace backends = agora::core::shell_backends;
namespace client = agora::core::shell_client;
namespace crypto = agora::core::crypto;
namespace durable = agora::core::durable_jobs;
namespace json = agora::core::json;
namespace openai = agora::core::openai;
namespace provider = agora::core::shell_provider;
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

json::Value strings_json(const std::vector<std::string>& values) {
    std::vector<json::Value> elements;
    elements.reserve(values.size());
    for (const std::string& value : values) elements.push_back(jstr(value));
    return jarr(std::move(elements));
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

// ---- The streaming seam (inert for this module) --------------------------------------------
//
// ShellToolProvider.kt routes every Conch command through ShellDurableJobExecutor, so the
// provider never calls HttpClient.streamPost (the backends stand pins that surface). The double
// records nothing and fails loudly if the provider ever reaches it.
class NoStreamTransport final : public backends::HttpStreamTransport {
public:
    bool closed = false;
    std::unique_ptr<backends::HttpStreamHandle> stream_post(
        const backends::StreamCall&) override {
        throw agora::core::test::Failure("provider double: unexpected stream_post");
    }
};

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
        std::string text;
        std::string value;
        std::int32_t number = 0;
        bool flag = false;
    };

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
    std::optional<shell_tools::ShellFileReadResult> file_read_result;
    std::optional<std::string> file_read_failure;
    std::optional<std::string> file_write_error;
    std::optional<std::string> file_write_failure;
    std::optional<shell_file::ShellFileEditResult> file_edit_result;
    std::optional<std::string> file_edit_failure;
    std::optional<std::pair<std::vector<std::string>, bool>> file_glob_result;
    std::optional<std::string> file_glob_failure;
    std::optional<backends::GrepOutcome> file_grep_result;
    std::optional<std::string> file_grep_failure;

    std::vector<std::string> calls;

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

    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit) override {
        calls.push_back("fileRead:" + path);
        if (file_read_failure) throw std::runtime_error(*file_read_failure);
        if (file_read_result) return *file_read_result;
        shell_tools::ShellFileReadResult result;
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

    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& /*old_string*/,
                                              const std::string& /*new_string*/,
                                              bool /*replace_all*/) override {
        calls.push_back("fileEdit:" + path);
        if (file_edit_failure) throw std::runtime_error(*file_edit_failure);
        return file_edit_result.value_or(shell_file::ShellFileEditResult{0, "", std::nullopt});
    }
};

// ---- Provider seams ------------------------------------------------------------------------

class TestBackendFactory final : public provider::BackendFactory {
public:
    ScriptedConchTransport conch;
    ScriptedSshTransport ssh;
    NoStreamTransport stream;

    std::unique_ptr<backends::ConchBackend> make_conch(
        const backends::ShellDeviceConfig& device) override {
        return std::make_unique<backends::ConchBackend>(device, conch, stream);
    }

    std::unique_ptr<backends::SshBackend> make_ssh(
        const backends::ShellDeviceConfig& device) override {
        return std::make_unique<backends::SshBackend>(device, ssh);
    }
};

class TestSandboxFactory final : public provider::SandboxFactory {
public:
    ScriptedSandboxManager manager;
    bool factory_available = true;

    backends::SandboxManager* create() override { return &manager; }
    bool is_available() override { return factory_available; }
};

class TestImageStore final : public provider::ToolImageStore {
public:
    std::optional<provider::ToolImageAttachment> result;
    std::optional<std::string> failure;
    std::vector<std::string> calls;

    provider::ToolImageAttachment persist_base64(const std::string& data,
                                                 const std::string& mime_type,
                                                 const std::string& file_prefix) override {
        calls.push_back("persistBase64:" + std::to_string(data.size()) + ":" + mime_type + ":" +
                        file_prefix);
        if (failure) throw std::runtime_error(*failure);
        if (result) return *result;
        provider::ToolImageAttachment attachment;
        attachment.path = "/data/user/0/com.newoether.agora/files/tool-media/" + file_prefix +
                          "_stand.png";
        attachment.mime_type = mime_type;
        attachment.size_bytes = static_cast<std::int64_t>(data.size()) * 3 / 4;
        attachment.width = 64;
        attachment.height = 32;
        attachment.sha256 = std::string(64, '0');
        return attachment;
    }
};

// ---- Scenario parsing ----------------------------------------------------------------------

backends::ShellDeviceConfig device_of(const json::Value& object, const std::string& stem) {
    backends::ShellDeviceConfig config;
    config.name = required_string(object, "name", stem);
    config.description = optional_string_or(object, "description");
    config.type = optional_string_or(object, "type", "conch");
    config.server_url = optional_string_or(object, "serverUrl");
    config.api_key = optional_string_or(object, "apiKey");
    config.conch_public_key = optional_string_or(object, "conchPublicKey");
    config.ssh_host = optional_string_or(object, "sshHost");
    config.ssh_port =
        static_cast<std::int32_t>(optional_integer(object, "sshPort").value_or(22));
    config.ssh_user = optional_string_or(object, "sshUser", "root");
    config.ssh_password = optional_string_or(object, "sshPassword");
    config.ssh_host_key = optional_string_or(object, "sshHostKey");
    return config;
}

provider::GenerationContext context_of(const json::Value& scenario, const std::string& stem) {
    provider::GenerationContext ctx;
    const json::Value* context = field(scenario, "context");
    if (context == nullptr || context->kind != json::Kind::Object) return ctx;
    if (const json::Value* conversation = field(*context, "conversationId")) {
        if (conversation->kind == json::Kind::String) {
            ctx.conversation_id = conversation->text;
        }
    }
    ctx.shell_enabled = optional_boolean(*context, "shellEnabled", false);
    ctx.sandbox_enabled = optional_boolean(*context, "sandboxEnabled", false);
    ctx.sandbox_shared_storage_enabled =
        optional_boolean(*context, "sandboxSharedStorageEnabled", false);
    ctx.tool_timeout_ms = optional_integer(*context, "toolTimeoutMs").value_or(60'000);
    const json::Value* devices = field(*context, "shellDevices");
    if (devices != nullptr && devices->kind == json::Kind::Array) {
        for (const json::Value& device : devices->elements) {
            if (device.kind != json::Kind::Object) scenario_fail(stem, "device is not an object");
            ctx.shell_devices.push_back(device_of(device, stem));
        }
    }
    return ctx;
}

std::string handshake_json(const std::string& api_key, const std::string& challenge) {
    const std::string public_key(kServerPublic);
    const std::string features(kRequiredFeatures);
    const std::string nonce = "stand-nonce";
    const std::string security_payload =
        "conch-handshake-v2|" + public_key + "|" + challenge + "|" + features;
    return json::to_kotlin_string(jobj({
        {"public_key", jstr(public_key)},
        {"nonce", jstr(nonce)},
        {"signature", jstr(client::hmac_hex(api_key, nonce + "|" + public_key))},
        {"features", jstr(features)},
        {"challenge", jstr(challenge)},
        {"features_signature",
         jstr(client::hmac_hex(api_key, nonce + "|" + security_payload))},
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

void install_conch_script(const json::Value& scenario, ScriptedConchTransport& transport) {
    transport.reset();
    const std::string api_key = optional_string_or(scenario, "conch_api_key");
    transport.handshake_builder = [api_key](const std::string& challenge) {
        return handshake_json(api_key, challenge);
    };
    if (!api_key.empty()) {
        ScriptedConchTransport::Step handshake;
        handshake.dynamic_handshake = true;
        transport.queue.push_back(handshake);
    }
    const json::Value* responses = field(scenario, "conch_responses");
    if (responses == nullptr || responses->kind != json::Kind::Array) return;
    for (const json::Value& element : responses->elements) {
        if (element.kind != json::Kind::Object) continue;
        ScriptedConchTransport::Step step;
        step.code = static_cast<std::int32_t>(optional_integer(element, "code").value_or(200));
        step.body = optional_string_or(element, "body");
        const json::Value* raw = field(element, "raw_body");
        if (raw != nullptr && raw->kind == json::Kind::String) step.raw_body = raw->text;
        step.signature_mode = optional_string_or(element, "signature", "absent");
        const json::Value* failure = field(element, "failure");
        if (failure != nullptr && failure->kind == json::Kind::String) {
            step.kind = ScriptedConchTransport::Step::Kind::Fail;
            step.failure_kind = failure_kind_of(failure->text);
            step.failure_detail = optional_string_or(element, "failure_detail");
        }
        transport.queue.push_back(step);
    }
}

void install_sandbox_script(const json::Value& scenario, TestSandboxFactory& factory) {
    const json::Value* script = field(scenario, "sandbox");
    if (script == nullptr || script->kind != json::Kind::Object) return;
    ScriptedSandboxManager& manager = factory.manager;
    factory.factory_available = optional_boolean(*script, "factory_available", true);
    manager.available = optional_boolean(*script, "available", true);
    if (const json::Value* failure = field(*script, "execute_failure")) {
        if (failure->kind == json::Kind::String) manager.execute_failure = failure->text;
    }
    if (const json::Value* result = field(*script, "execute_result")) {
        if (result->kind == json::Kind::Object) {
            backends::SandboxManager::SandboxResult value;
            value.stdout_text = optional_string_or(*result, "stdout");
            value.stderr_text = optional_string_or(*result, "stderr");
            value.exit_code =
                static_cast<std::int32_t>(optional_integer(*result, "exit_code").value_or(0));
            const json::Value* warning = field(*result, "warning");
            if (warning != nullptr && warning->kind == json::Kind::String) {
                value.warning = warning->text;
            }
            manager.execute_result = value;
        }
    }
    if (const json::Value* failure = field(*script, "file_read_failure")) {
        if (failure->kind == json::Kind::String) manager.file_read_failure = failure->text;
    }
    if (const json::Value* result = field(*script, "file_read_result")) {
        if (result->kind == json::Kind::Object) {
            shell_tools::ShellFileReadResult value;
            value.content = optional_string_or(*result, "content");
            value.lines =
                static_cast<std::int32_t>(optional_integer(*result, "lines").value_or(0));
            value.total_lines =
                static_cast<std::int32_t>(optional_integer(*result, "total_lines").value_or(0));
            value.total_bytes = optional_integer(*result, "total_bytes").value_or(0);
            value.returned_bytes = optional_integer(*result, "returned_bytes").value_or(0);
            value.offset = optional_integer(*result, "offset").value_or(0);
            value.limit = optional_integer(*result, "limit").value_or(0);
            value.truncated = optional_boolean(*result, "truncated", false);
            const json::Value* error = field(*result, "error");
            if (error != nullptr && error->kind == json::Kind::String) value.error = error->text;
            manager.file_read_result = value;
        }
    }
    if (const json::Value* failure = field(*script, "file_write_failure")) {
        if (failure->kind == json::Kind::String) manager.file_write_failure = failure->text;
    }
    if (const json::Value* error = field(*script, "file_write_error")) {
        if (error->kind == json::Kind::String) manager.file_write_error = error->text;
    }
    if (const json::Value* failure = field(*script, "file_edit_failure")) {
        if (failure->kind == json::Kind::String) manager.file_edit_failure = failure->text;
    }
    if (const json::Value* result = field(*script, "file_edit_result")) {
        if (result->kind == json::Kind::Object) {
            shell_file::ShellFileEditResult value;
            value.replacements =
                static_cast<int>(optional_integer(*result, "replacements").value_or(0));
            value.sha256 = optional_string_or(*result, "sha256");
            const json::Value* error = field(*result, "error");
            if (error != nullptr && error->kind == json::Kind::String) value.error = error->text;
            manager.file_edit_result = value;
        }
    }
    if (const json::Value* failure = field(*script, "file_glob_failure")) {
        if (failure->kind == json::Kind::String) manager.file_glob_failure = failure->text;
    }
    if (const json::Value* result = field(*script, "file_glob_result")) {
        if (result->kind == json::Kind::Object) {
            manager.file_glob_result =
                std::make_pair(strings_of(*result, "files"),
                               optional_boolean(*result, "truncated", false));
        }
    }
    if (const json::Value* failure = field(*script, "file_grep_failure")) {
        if (failure->kind == json::Kind::String) manager.file_grep_failure = failure->text;
    }
    if (const json::Value* result = field(*script, "file_grep_result")) {
        if (result->kind == json::Kind::Object) {
            backends::GrepOutcome value;
            value.ok = true;
            const json::Value* matches = field(*result, "matches");
            if (matches != nullptr && matches->kind == json::Kind::Array) {
                for (const json::Value& element : matches->elements) {
                    if (element.kind != json::Kind::Object) continue;
                    backends::GrepMatch match;
                    match.path = optional_string_or(element, "path");
                    match.line =
                        static_cast<std::int32_t>(optional_integer(element, "line").value_or(0));
                    match.content = optional_string_or(element, "content");
                    value.matches.push_back(std::move(match));
                }
            }
            value.truncated = optional_boolean(*result, "truncated", false);
            manager.file_grep_result = value;
        }
    }
}

void install_image_script(const json::Value& scenario, TestImageStore& store) {
    const json::Value* script = field(scenario, "image");
    if (script == nullptr || script->kind != json::Kind::Object) return;
    if (const json::Value* failure = field(*script, "failure")) {
        if (failure->kind == json::Kind::String) store.failure = failure->text;
    }
    if (const json::Value* result = field(*script, "result")) {
        if (result->kind == json::Kind::Object) {
            provider::ToolImageAttachment attachment;
            attachment.path = optional_string_or(*result, "path");
            attachment.mime_type = optional_string_or(*result, "mime_type");
            attachment.size_bytes = optional_integer(*result, "size").value_or(0);
            const auto width = optional_integer(*result, "width");
            const auto height = optional_integer(*result, "height");
            if (width) attachment.width = static_cast<std::int32_t>(*width);
            if (height) attachment.height = static_cast<std::int32_t>(*height);
            attachment.sha256 = optional_string_or(*result, "sha256");
            store.result = attachment;
        }
    }
}

void install_jsch_script(const json::Value& scenario, ScriptedSshTransport& ssh) {
    ssh.reset();
    const json::Value* script = field(scenario, "jsch");
    if (script == nullptr || script->kind != json::Kind::Object) return;
    ssh.presented_key = optional_string_or(*script, "presented_key");
    if (const json::Value* value = field(*script, "strict_rejection")) {
        if (value->kind == json::Kind::String) ssh.strict_rejection_message = value->text;
    }
    if (const json::Value* value = field(*script, "connect_failure")) {
        if (value->kind == json::Kind::String) ssh.connect_failure = value->text;
    }
    if (const json::Value* value = field(*script, "open_channel_failure")) {
        if (value->kind == json::Kind::String) ssh.open_channel_failure = value->text;
    }
    if (const auto value = optional_integer(*script, "exit_status")) {
        ssh.exit_status = static_cast<std::int32_t>(*value);
    }
    ssh.never_close = optional_boolean(*script, "never_close", false);
    ssh.stdout_bytes = optional_string_or(*script, "stdout");
    ssh.stderr_bytes = optional_string_or(*script, "stderr");
    ssh.pwd_path = optional_string_or(*script, "pwd", "/home/user");

    if (const json::Value* stats = field(*script, "stats")) {
        if (stats->kind == json::Kind::Object) {
            for (const json::Member& member : stats->members) {
                if (member.value.kind != json::Kind::Object) continue;
                backends::SshTransport::SftpAttrs attrs;
                attrs.is_dir = optional_boolean(member.value, "is_dir", false);
                attrs.size = optional_integer(member.value, "size").value_or(0);
                ssh.stats[member.key] = attrs;
            }
        }
    }
    if (const json::Value* failures = field(*script, "mkdir_failures")) {
        if (failures->kind == json::Kind::Array) {
            for (const json::Value& element : failures->elements) {
                ssh.mkdir_failures.insert(element.text);
            }
        }
    }
    if (const json::Value* failures = field(*script, "put_failures")) {
        if (failures->kind == json::Kind::Object) {
            for (const json::Member& member : failures->members) {
                if (member.value.kind == json::Kind::String) {
                    ssh.put_failures[member.key] = member.value.text;
                }
            }
        }
    }
    if (const json::Value* files = field(*script, "file_bytes")) {
        if (files->kind == json::Kind::Object) {
            for (const json::Member& member : files->members) {
                if (member.value.kind == json::Kind::String) {
                    ssh.file_bytes[member.key] = member.value.text;
                }
            }
        }
    }
    if (const json::Value* files = field(*script, "file_bytes_second")) {
        if (files->kind == json::Kind::Object) {
            for (const json::Member& member : files->members) {
                if (member.value.kind == json::Kind::String) {
                    ssh.file_bytes_second[member.key] = member.value.text;
                }
            }
        }
    }
    if (const json::Value* listing = field(*script, "ls")) {
        if (listing->kind == json::Kind::Object) {
            for (const json::Member& member : listing->members) {
                if (member.value.kind != json::Kind::Array) continue;
                std::vector<backends::SshTransport::LsEntry> entries;
                for (const json::Value& element : member.value.elements) {
                    if (element.kind != json::Kind::Array || element.elements.size() != 2) {
                        continue;
                    }
                    backends::SshTransport::LsEntry entry;
                    entry.filename = element.elements[0].text;
                    entry.is_dir = element.elements[1].text == "true";
                    entries.push_back(std::move(entry));
                }
                ssh.ls_entries[member.key] = std::move(entries);
            }
        }
    }
}

// ---- Recorded transports → trace JSON ------------------------------------------------------

std::string mask_header(const std::string& key, const std::string& value) {
    if (key == "X-Timestamp") return "<timestamp>";
    if (key == "X-Signature") return "<signature>";
    if (key == "X-Nonce") return "<nonce>";
    if (key == "X-Client-Public-Key") return "<client-public-key>";
    return value;
}

std::string mask_url(const std::string& url) {
    const std::string needle = "challenge=";
    const std::size_t at = url.find(needle);
    if (at == std::string::npos) return url;
    std::size_t end = url.find('&', at);
    if (end == std::string::npos) end = url.size();
    return url.substr(0, at) + "challenge=<challenge>" + url.substr(end);
}

json::Value conch_requests_json(const ScriptedConchTransport& transport) {
    std::vector<json::Value> requests;
    for (const ScriptedConchTransport::Request& request : transport.requests) {
        const bool encrypted =
            std::any_of(request.headers.begin(), request.headers.end(),
                        [](const auto& header) { return header.first == "X-Encryption"; });
        std::vector<json::Value> headers;
        for (const auto& [key, value] : request.headers) {
            headers.push_back(jarr({jstr(key), jstr(mask_header(key, value))}));
        }
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("url", jstr(mask_url(request.url)));
        members.emplace_back("method", jstr(request.method));
        members.emplace_back("body", jstr(encrypted ? "<encrypted>" : request.body));
        members.emplace_back("headers", jarr(std::move(headers)));
        if (request.call_timeout_millis) {
            members.emplace_back("call_timeout_ms",
                                 jnum(std::to_string(*request.call_timeout_millis)));
        } else {
            members.emplace_back("call_timeout_ms", jnull());
        }
        requests.push_back(jobj(std::move(members)));
    }
    return jarr(std::move(requests));
}

json::Value ssh_events_json(const ScriptedSshTransport& ssh) {
    std::vector<json::Value> events;
    for (const ScriptedSshTransport::Event& event : ssh.events) {
        switch (event.kind) {
            case ScriptedSshTransport::Event::Kind::SetPassword:
                events.push_back(
                    jobj({{"event", jstr("set_password")}, {"password", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::SetConfig:
                events.push_back(jobj({{"event", jstr("set_config")},
                                       {"key", jstr(event.text)},
                                       {"value", jstr(event.value)}}));
                break;
            case ScriptedSshTransport::Event::Kind::HostKeyCheck:
                events.push_back(jobj({{"event", jstr("host_key_check")},
                                       {"host", jstr(event.text)},
                                       {"decision", jnum(std::to_string(event.number))}}));
                break;
            case ScriptedSshTransport::Event::Kind::Connect:
                events.push_back(jobj({{"event", jstr("connect")},
                                       {"channel", jstr(event.text)},
                                       {"timeout_ms", jnum(std::to_string(event.number))}}));
                break;
            case ScriptedSshTransport::Event::Kind::OpenChannel:
                events.push_back(
                    jobj({{"event", jstr("open_channel")}, {"kind", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::SetCommand:
                events.push_back(
                    jobj({{"event", jstr("set_command")}, {"command", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::Disconnect:
                events.push_back(
                    jobj({{"event", jstr("disconnect")}, {"channel", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::Stat:
                events.push_back(jobj({{"event", jstr("stat")},
                                       {"path", jstr(event.text)},
                                       {"found", jbool(event.flag)}}));
                break;
            case ScriptedSshTransport::Event::Kind::Mkdir:
                events.push_back(
                    jobj({{"event", jstr("mkdir")}, {"path", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::Get:
                events.push_back(jobj({{"event", jstr("get")}, {"path", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::Put:
                events.push_back(jobj({{"event", jstr("put")},
                                       {"path", jstr(event.text)},
                                       {"bytes", jnum(std::to_string(event.number))}}));
                break;
            case ScriptedSshTransport::Event::Kind::Ls:
                events.push_back(jobj({{"event", jstr("ls")}, {"dir", jstr(event.text)}}));
                break;
            case ScriptedSshTransport::Event::Kind::Pwd:
                events.push_back(jobj({{"event", jstr("pwd")}, {"path", jstr(event.text)}}));
                break;
        }
    }
    return jarr(std::move(events));
}

json::Value image_json(const provider::ToolImageAttachment& image) {
    return jobj({
        {"path", jstr(image.path)},
        {"mime_type", jstr(image.mime_type)},
        {"size", jnum(std::to_string(image.size_bytes))},
        {"width", image.width ? jnum(std::to_string(*image.width)) : jnull()},
        {"height", image.height ? jnum(std::to_string(*image.height)) : jnull()},
        {"sha256", jstr(image.sha256)},
    });
}

json::Value event_json(const provider::ToolExecutionEvent& event) {
    using Kind = provider::ToolExecutionEvent::Kind;
    switch (event.kind) {
        case Kind::OutputDelta:
            return jobj({{"type", jstr("delta")}, {"text", jstr(event.text)}});
        case Kind::OutputSnapshot:
            return jobj({{"type", jstr("snapshot")}, {"text", jstr(event.text)}});
        case Kind::TargetResolved:
            return jobj({{"type", jstr("target_resolved")}, {"target", jstr(event.text)}});
        case Kind::Progress:
            return jobj({{"type", jstr("progress")}, {"message", jstr(event.text)}});
        case Kind::Completed: {
            std::vector<json::Value> images;
            images.reserve(event.images.size());
            for (const provider::ToolImageAttachment& image : event.images) {
                images.push_back(image_json(image));
            }
            return jobj({
                {"type", jstr("completed")},
                {"text", jstr(event.text)},
                {"is_error", jbool(event.is_error)},
                {"images", jarr(std::move(images))},
            });
        }
    }
    return jnull();
}

json::Value definitions_json(const provider::ShellToolProvider& shell_provider,
                             const provider::GenerationContext& ctx) {
    std::vector<json::Value> definitions;
    for (const openai::ToolDefinition& definition : shell_provider.definitions(ctx)) {
        std::vector<json::Value> properties;
        for (const auto& property : definition.function.parameters.properties) {
            properties.push_back(jstr(property.first));
        }
        std::vector<json::Value> required;
        for (const std::string& name : definition.function.parameters.required) {
            required.push_back(jstr(name));
        }
        definitions.push_back(jobj({
            {"name", jstr(definition.function.name)},
            {"properties", jarr(std::move(properties))},
            {"required", jarr(std::move(required))},
        }));
    }
    return jarr(std::move(definitions));
}

// ---- Outcomes ------------------------------------------------------------------------------

std::pair<std::string, std::string> tool_call(const json::Value& scenario,
                                              const std::string& stem) {
    const json::Value* call = field(scenario, "tool_call");
    if (call == nullptr || call->kind != json::Kind::Object) {
        scenario_fail(stem, "missing tool_call");
    }
    return {required_string(*call, "name", stem),
            optional_string_or(*call, "arguments", "{}")};
}

json::Value execute_outcome(provider::ShellToolProvider& shell_provider,
                            const json::Value& scenario, const provider::GenerationContext& ctx,
                            const std::string& stem) {
    const auto [name, arguments] = tool_call(scenario, stem);
    std::optional<std::string> result;
    std::optional<std::string> threw;
    try {
        result = shell_provider.execute(name, arguments, ctx);
    } catch (const std::exception& error) {
        threw = error.what();
    }
    return jobj({{"result", optional_result(result)}, {"threw", optional_result(threw)}});
}

json::Value events_outcome(provider::ShellToolProvider& shell_provider,
                           const json::Value& scenario, const provider::GenerationContext& ctx,
                           const std::string& stem) {
    const auto [name, arguments] = tool_call(scenario, stem);
    std::vector<json::Value> events;
    std::optional<std::string> failure;
    try {
        shell_provider.execute_events(
            name, arguments, ctx,
            [&events](const provider::ToolExecutionEvent& event) {
                events.push_back(event_json(event));
            });
    } catch (const std::exception& error) {
        failure = error.what();
    }
    return jobj({
        {"events", failure ? jnull() : jarr(std::move(events))},
        {"threw", optional_result(failure)},
    });
}

json::Value acknowledge_outcome(provider::ShellToolProvider& shell_provider,
                                const json::Value& scenario,
                                const provider::GenerationContext& ctx,
                                const std::string& stem) {
    std::vector<durable::ToolCallRecord> calls;
    const json::Value* array = field(scenario, "calls");
    if (array != nullptr && array->kind == json::Kind::Array) {
        for (const json::Value& element : array->elements) {
            if (element.kind != json::Kind::Object) continue;
            durable::ToolCallRecord record;
            record.tool_name = required_string(element, "toolName", stem);
            record.arguments = required_string(element, "arguments", stem);
            record.result = required_string(element, "result", stem);
            calls.push_back(std::move(record));
        }
    }
    std::optional<std::string> threw;
    try {
        shell_provider.acknowledge_committed_jobs(calls, ctx);
    } catch (const std::exception& error) {
        threw = error.what();
    }
    return jobj({
        {"result", threw ? jnull() : jstr("acknowledged")},
        {"threw", optional_result(threw)},
    });
}

// ---- Trace assembly ------------------------------------------------------------------------

std::string mask_waited_ms(std::string text) {
    const std::string needle = "waited_ms";
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        std::size_t cursor = position + needle.size();
        if (cursor < text.size() && text[cursor] == '\\') ++cursor;
        if (cursor + 1 < text.size() && text[cursor] == '"' && text[cursor + 1] == ':') {
            cursor += 2;
            const std::size_t digits = cursor;
            while (cursor < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
            if (cursor > digits) {
                text.replace(digits, cursor - digits, "MASKED");
                position = digits + 6;
                continue;
            }
        }
        position = cursor;
    }
    return text;
}

std::string render(const json::Value& scenario, const std::string& stem) {
    const std::string op = required_string(scenario, "op", stem);
    const provider::GenerationContext ctx = context_of(scenario, stem);

    TestBackendFactory factory;
    TestSandboxFactory sandbox_factory;
    TestImageStore image_store;
    install_conch_script(scenario, factory.conch);
    install_sandbox_script(scenario, sandbox_factory);
    install_image_script(scenario, image_store);
    install_jsch_script(scenario, factory.ssh);

    std::int64_t clock = 0;
    durable::DurableJobHooks hooks{
        [&clock] { return clock; },
        [&clock](std::int64_t millis) { clock += millis; },
        [] { return true; },
    };
    const bool no_image_store = optional_boolean(scenario, "no_image_store", false);
    provider::ShellToolProvider shell_provider(factory, &sandbox_factory,
                                               no_image_store ? nullptr : &image_store, hooks);
    std::vector<json::Value> confirm_calls;
    const bool allowed = optional_boolean(scenario, "confirm_allowed", true);
    shell_provider.set_confirm(
        [&confirm_calls, allowed](const std::string& server, const std::string& summary,
                                  const std::optional<std::string>& conversation_id) {
            confirm_calls.push_back(jobj({
                {"server", jstr(server)},
                {"summary", jstr(summary)},
                {"conversation_id", conversation_id ? jstr(*conversation_id) : jnull()},
            }));
            return allowed;
        });

    json::Value outcome = jnull();
    if (op == "execute") {
        outcome = execute_outcome(shell_provider, scenario, ctx, stem);
    } else if (op == "events") {
        outcome = events_outcome(shell_provider, scenario, ctx, stem);
    } else if (op == "acknowledge") {
        outcome = acknowledge_outcome(shell_provider, scenario, ctx, stem);
    } else if (op == "definitions") {
        outcome = definitions_json(shell_provider, ctx);
    } else {
        scenario_fail(stem, "unknown op: " + op);
    }

    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("op", jstr(op));
    members.emplace_back("outcome", std::move(outcome));
    if (op != "definitions") {
        members.emplace_back("definitions", definitions_json(shell_provider, ctx));
    }
    members.emplace_back("confirm_calls", jarr(std::move(confirm_calls)));
    members.emplace_back("conch_requests", conch_requests_json(factory.conch));
    members.emplace_back("http_calls", jarr({}));
    members.emplace_back("http_closed", jbool(factory.stream.closed));
    members.emplace_back("sandbox_calls", strings_json(sandbox_factory.manager.calls));
    members.emplace_back("image_calls", strings_json(image_store.calls));
    members.emplace_back("ssh_events", ssh_events_json(factory.ssh));
    return mask_waited_ms(json::to_kotlin_string(jobj(std::move(members))));
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

TEST(shell_tool_provider_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_TOOL_PROVIDER_DIR;
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
        const std::string actual = render(*parsed.value, stem) + "\n";
        const std::string expected = read_file(golden_dir / (stem + ".trace"));
        CHECK(!expected.empty());
        if (actual != expected) {
            throw agora::core::test::Failure(stem + ": " + describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
