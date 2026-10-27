#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/shell_client.hpp"
#include "agora/core/shell_file_edit.hpp"
#include "agora/core/shell_tools.hpp"

namespace agora::core::shell_backends {

// tool/ShellToolBackends.kt: the Backend abstraction and its three implementations.
//
//   ConchBackend   — ShellClient request assembly + the streaming /execute SSE session;
//   SshBackend     — SshClient exec/SFTP operations with the SSH output and error formatting;
//   SandboxBackend — the local proot sandbox manager with its warning/error formatting.
//
// Every file operation that fails in transport produces the shared failedFileRead result (an
// empty read carrying the error), while execute failures produce the exact jsonError payloads.
//
// Platform boundaries (each pinned by the golden stand's scripted double):
//   * HttpStreamTransport — the api/HttpClient.kt streaming surface. The app adapter owns OkHttp
//     and the request scope/call factory plumbing; the port pins the invocation (URL, prepared
//     body, prepared headers, ConchNetwork's line/control limits), the HTTP-status branch, the
//     bounded error-body detail and the SSE read loop.
//   * ConchTransport — ShellClient's GET/POST transport, already pinned by the shell-client
//     module and reused here unchanged.
//   * SshTransport — the JSch surface SshClient.kt drives: session configuration, the host-key
//     handshake (the transport calls back into SshClient's trust-on-first-use policy and applies
//     StrictHostKeyChecking), the exec channel's isClosed/exitStatus wait loop, and the SFTP
//     channel's stat/mkdir/get/put/pwd/ls. The real adapter owns JSch; the stand scripts it.
//   * SandboxManager — the proot sandbox operations; the real implementation stays on the
//     platform, the stand scripts it.
//
// Coroutine mechanics (Dispatchers.IO, cancellation, delay) are platform concerns: the core is
// synchronous and blocking by construction. The exec wait loop takes its clock from the
// transport so the stand can pin the timeout without a wall-clock sleep.

// ---------------------------------------------------------------------------------------------
// Shared result models
// ---------------------------------------------------------------------------------------------

/** ShellClient.GrepMatch (SshClient.GrepMatch and SandboxManager.GrepMatch have the same shape). */
struct GrepMatch {
    std::string path;
    std::int32_t line = 0;
    std::string content;
};

/** Kotlin `Result<Pair<List<String>, Boolean>>` folded into a value. */
struct GlobOutcome {
    bool ok = false;
    std::vector<std::string> files;
    bool truncated = false;
    std::string error;
};

/** Kotlin `Result<Pair<List<GrepMatch>, Boolean>>` folded into a value. */
struct GrepOutcome {
    bool ok = false;
    std::vector<GrepMatch> matches;
    bool truncated = false;
    std::string error;
};

/**
 * failedFileRead: the empty ShellFileReadResult carrying `error`, with the Kotlin coercions
 * `offset.coerceAtLeast(0)` and `limit in 1..SHELL_FILE_READ_MAX_BYTES` (else the 1 MiB cap).
 */
shell_tools::ShellFileReadResult failed_file_read(std::int64_t offset, std::int64_t limit,
                                                  std::string error);

// ---------------------------------------------------------------------------------------------
// ShellDeviceConfig — the full com.newoether.agora.data.ShellDeviceConfig leaf the backends read.
// shell_tools::ShellDeviceConfig (name/type) stays the lean definitions input; the provider maps
// between the two.
// ---------------------------------------------------------------------------------------------

struct ShellDeviceConfig {
    std::string name;
    std::string description;
    std::string type = "conch";  // "conch" | "ssh"
    std::string server_url;
    std::string api_key;
    std::string conch_public_key;
    std::string ssh_host;
    std::int32_t ssh_port = 22;
    std::string ssh_user = "root";
    std::string ssh_password;
    std::string ssh_host_key;
};

// ---------------------------------------------------------------------------------------------
// SSH transport boundary + SshClient port
// ---------------------------------------------------------------------------------------------

/** Kotlin SshClient.CommandResult. */
struct SshCommandResult {
    std::string stdout_text;
    std::string stderr_text;
    std::int32_t exit_code = 0;
};

/** The JSch HostKeyRepository callback SshClient's trust-on-first-use repository implements. */
class HostKeyPolicy {
public:
    enum Decision : int {
        Ok = 0,           // HostKeyRepository.OK
        Changed = 1,      // HostKeyRepository.CHANGED
        NotIncluded = 2,  // HostKeyRepository.NOT_INCLUDED
    };
    virtual ~HostKeyPolicy() = default;

    /** Receives the presented host-key blob (the JSch `key` byte array) and returns a decision. */
    virtual int check(const std::string& host, const std::vector<std::uint8_t>& key) = 0;
};

/**
 * The JSch surface util/SshClient.kt drives. One instance backs one SshClient (Kotlin creates a
 * fresh JSch per client). Methods throw on the failures the Kotlin code observes.
 */
class SshTransport {
public:
    struct SftpAttrs {
        bool is_dir = false;
        std::int64_t size = 0;
    };
    struct LsEntry {
        std::string filename;
        bool is_dir = false;
    };

    virtual ~SshTransport() = default;

    // JSch.getSession(user, host, port) + Session.apply { setPassword; setConfig(...);
    // hostKeyRepository = ...; connect }.
    /** Starts a new session object (nothing is sent yet). */
    virtual void open_session(const std::string& user, const std::string& host,
                              std::int32_t port) = 0;
    virtual void set_password(const std::string& password) = 0;
    virtual void set_config(const std::string& key, const std::string& value) = 0;
    /**
     * Session connect. Must present the server's host-key blob to @p policy, then enforce the
     * configured StrictHostKeyChecking value: "yes" accepts only Ok and throws on
     * Changed/NotIncluded; "no" accepts any decision (capture mode). Throws on transport
     * failures before the host-key exchange.
     */
    virtual void connect(std::int32_t timeout_ms, HostKeyPolicy& policy) = 0;
    virtual bool is_connected() = 0;
    virtual void disconnect_session() = 0;

    // ChannelExec (openChannel("exec") as ChannelExec).
    virtual void open_exec() = 0;
    virtual void set_command(const std::string& command) = 0;
    /** Connects the channel and collects the command's stdout/stderr into the channel buffers. */
    virtual void connect_exec(std::int32_t timeout_ms) = 0;
    virtual bool exec_is_closed() = 0;
    virtual std::int32_t exec_exit_status() = 0;
    /** Raw stdout bytes (ByteArrayOutputStream); SshClient decodes them with the JVM's REPLACE
     *  UTF-8 decoder, exactly like `toString("UTF-8")`. */
    virtual std::string exec_stdout() = 0;
    virtual std::string exec_stderr() = 0;
    virtual void disconnect_exec() = 0;

    // ChannelSftp (openChannel("sftp") as ChannelSftp).
    virtual void open_sftp() = 0;
    virtual void connect_sftp(std::int32_t timeout_ms) = 0;
    virtual SftpAttrs stat(const std::string& path) = 0;
    virtual void mkdir(const std::string& path) = 0;
    /** Returns the file's bytes; each call starts a fresh stream (like ChannelSftp.get). */
    virtual std::string get(const std::string& path) = 0;
    virtual void put(const std::string& path, std::string_view bytes) = 0;
    virtual std::string pwd() = 0;
    virtual std::vector<LsEntry> ls(const std::string& dir) = 0;
    virtual void disconnect_sftp() = 0;

    // Clock hooks for the exec completion loop (System.currentTimeMillis + kotlinx delay(100)).
    virtual std::int64_t clock_millis() = 0;
    virtual void sleep_millis(std::int64_t millis) = 0;
};

/**
 * The util/SshClient.kt port: the TOFU host-key repository, the exec command assembly
 * (`cd <escaped workdir> && command`), the stdout/stderr formatting inputs, the bounded SFTP
 * file read with its complete-content re-read, write with best-effort parent mkdirs, edit with
 * the concurrent-change guard, glob with the pwd fallback and PortableGlobMatcher semantics, and
 * grep with the server-side command / SFTP fallback strategy.
 */
class SshClient {
public:
    SshClient(SshTransport& transport, std::string host, std::int32_t port, std::string user,
              std::string password, std::int32_t timeout_ms = 30'000,
              std::string pinned_host_key = "", bool allow_unknown_host_key = false);

    /** The server host key (standard base64 of the blob) seen on the last connect. */
    const std::optional<std::string>& captured_host_key() const noexcept {
        return captured_host_key_;
    }

    /** OpenSSH-style "SHA256:…" fingerprint; "" when the input does not decode. */
    static std::string fingerprint_sha256(const std::string& base64_key);

    /** escapeBash: single-quote wrapping with embedded quotes as '\''. */
    static std::string escape_bash(const std::string& text);

    /** Session disconnect (Kotlin close()); a session that never connected is a no-op. */
    void close();

    /** Throws std::runtime_error on transport failures and the exec timeout. */
    SshCommandResult execute_command(const std::string& command, const std::string& workdir,
                                     std::int32_t exec_timeout_ms);

    /**
     * Kotlin fileRead. Throws std::runtime_error("SFTP read failed: …") on failure; the backend
     * converts it to failedFileRead.
     */
    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit);

    /** Kotlin fileWrite: null on success, the "SFTP write failed: …" string on failure. */
    std::optional<std::string> file_write(const std::string& path, const std::string& content);

    /** Kotlin fileEdit: replacements/sha256 or the exact error string. */
    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& old_string,
                                              const std::string& new_string, bool replace_all);

    std::pair<std::vector<std::string>, bool> file_glob(
        const std::string& pattern, const std::string& base_path,
        const std::optional<std::int32_t>& depth);

    /**
     * Kotlin fileGrep. The server-side command runs first (exit 0/1 parse its stdout; any other
     * exit or exception falls back to the SFTP read+grep). The fallback compiles the pattern
     * with std::regex (ECMAScript, the core's established dialect); an invalid pattern is
     * matched literally, mirroring java.util.regex.Pattern.quote.
     */
    GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                          const std::string& file_glob);

private:
    class TofuPolicy;

    void ensure_session();
    int check_host_key(const std::string& host, const std::vector<std::uint8_t>& key);

    void ensure_parent_dirs(const std::string& path);
    void sftp_list_recursive(const std::string& dir, std::vector<std::string>& result,
                             std::int32_t remaining);
    static std::vector<std::string> glob_match(const std::vector<std::string>& files,
                                               const std::string& base_path,
                                               const std::string& pattern);
    GrepOutcome fallback_grep(const std::string& regex, const std::string& base_path,
                              const std::string& file_glob);
    static std::optional<GrepMatch> parse_grep_line(const std::string& line);

    SshTransport& transport_;
    std::string host_;
    std::int32_t port_;
    std::string user_;
    std::string password_;
    std::int32_t timeout_ms_;
    std::string pinned_host_key_;
    bool allow_unknown_host_key_;
    bool session_established_ = false;
    std::optional<std::string> captured_host_key_;
};

// ---------------------------------------------------------------------------------------------
// Sandbox transport boundary
// ---------------------------------------------------------------------------------------------

/** The SandboxManager surface SandboxBackend drives (proot stays a platform implementation). */
class SandboxManager {
public:
    /** Kotlin SandboxManager.SandboxResult. */
    struct SandboxResult {
        std::string stdout_text;
        std::string stderr_text;
        std::int32_t exit_code = 0;
        std::optional<std::string> warning;
    };

    virtual ~SandboxManager() = default;

    virtual bool is_available() = 0;
    virtual SandboxResult execute_command(const std::string& command, const std::string& workdir,
                                          std::int32_t timeout_ms) = 0;
    virtual shell_tools::ShellFileReadResult file_read(const std::string& path,
                                                       std::int64_t offset,
                                                       std::int64_t limit) = 0;
    virtual std::optional<std::string> file_write(const std::string& path,
                                                  const std::string& content) = 0;
    virtual shell_file::ShellFileEditResult file_edit(const std::string& path,
                                                      const std::string& old_string,
                                                      const std::string& new_string,
                                                      bool replace_all) = 0;
    virtual std::pair<std::vector<std::string>, bool> file_glob(
        const std::string& pattern, const std::string& base_path,
        const std::optional<std::int32_t>& depth) = 0;
    /** Kotlin Result<Pair<List<GrepMatch>, Boolean>>; failure carries the exception message. */
    virtual GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                                  const std::string& file_glob) = 0;
};

// ---------------------------------------------------------------------------------------------
// The Backend abstraction
// ---------------------------------------------------------------------------------------------

/** One emitted execution event (the backends only produce OutputDelta and Completed). */
struct BackendEvent {
    enum class Kind { OutputDelta, Completed };
    Kind kind = Kind::Completed;
    std::string text;
    bool is_error = false;  // Completed only (ToolExecutionResult.isError)
};

using BackendEventSink = std::function<void(const BackendEvent&)>;

/** tool/ShellToolBackends.kt's `sealed interface Backend`. */
class Backend {
public:
    virtual ~Backend() = default;

    /** The remote device, or nullopt for the local sandbox (never confirmation-gated). */
    virtual const std::optional<ShellDeviceConfig>& device() const = 0;

    virtual std::string execute_command(const std::string& command, const std::string& workdir,
                                        std::int32_t timeout_ms) = 0;

    /** Default contract: one Completed carrying the one-shot result (no deltas). */
    virtual void execute_command_events(const std::string& command, const std::string& workdir,
                                        std::int32_t timeout_ms, const BackendEventSink& sink) {
        sink(BackendEvent{BackendEvent::Kind::Completed,
                          execute_command(command, workdir, timeout_ms), false});
    }

    virtual shell_tools::ShellFileReadResult file_read(const std::string& path,
                                                       std::int64_t offset,
                                                       std::int64_t limit) = 0;
    virtual std::optional<std::string> file_write(const std::string& path,
                                                  const std::string& content) = 0;
    virtual shell_file::ShellFileEditResult file_edit(const std::string& path,
                                                      const std::string& old_string,
                                                      const std::string& new_string,
                                                      bool replace_all) = 0;
    virtual GlobOutcome file_glob(const std::string& pattern, const std::string& base_path,
                                  const std::optional<std::int32_t>& depth) = 0;
    virtual GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                                  const std::string& file_glob) = 0;
    virtual void close() = 0;
};

// ---------------------------------------------------------------------------------------------
// Streaming HTTP boundary (api/HttpClient.kt)
// ---------------------------------------------------------------------------------------------

/** util/ConchNetwork.kt's LINE_LIMIT / CONTROL_LIMIT, passed into every streamed request. */
inline constexpr std::int64_t kLineLimit = 10LL << 20;
inline constexpr std::int64_t kControlLimit = 64LL << 10;

/** One streamPost invocation (scope/callClient are platform plumbing, not pinned). */
struct StreamCall {
    std::string url;
    std::string json_body;
    shell_client::HeaderList headers;
    std::int64_t max_line_bytes = kLineLimit;
    std::int64_t max_error_bytes = kControlLimit;
};

/** api/HttpClient.kt's StreamHandle: code/errorBody are known once the response headers arrive. */
class HttpStreamHandle {
public:
    virtual ~HttpStreamHandle() = default;
    virtual std::int32_t code() const = 0;
    virtual std::optional<std::string> error_body() = 0;
    /** Next SSE line; nullopt at end of stream. */
    virtual std::optional<std::string> read_line() = 0;
    virtual void close() = 0;
};

/** Throws shell_client::TransportFailure on transport errors. */
class HttpStreamTransport {
public:
    virtual ~HttpStreamTransport() = default;
    virtual std::unique_ptr<HttpStreamHandle> stream_post(const StreamCall& call) = 0;
};

// ---------------------------------------------------------------------------------------------
// ConchBackend
// ---------------------------------------------------------------------------------------------

class ConchBackend final : public Backend {
public:
    ConchBackend(ShellDeviceConfig device, shell_client::ConchTransport& transport,
                 HttpStreamTransport& stream_transport);

    const std::optional<ShellDeviceConfig>& device() const override { return device_; }

    std::string execute_command(const std::string& command, const std::string& workdir,
                                std::int32_t timeout_ms) override;
    void execute_command_events(const std::string& command, const std::string& workdir,
                                std::int32_t timeout_ms,
                                const BackendEventSink& sink) override;

    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit) override;
    std::optional<std::string> file_write(const std::string& path,
                                          const std::string& content) override;
    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& old_string,
                                              const std::string& new_string,
                                              bool replace_all) override;
    GlobOutcome file_glob(const std::string& pattern, const std::string& base_path,
                          const std::optional<std::int32_t>& depth) override;
    GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                          const std::string& file_glob) override;
    void close() override {}

    // The durable-job and image endpoints the shell tool provider drives through this backend.
    // These are pure ShellClient delegations (pinned by the shell-client traces).
    std::string start_job(const std::string& command, std::int32_t timeout_ms,
                          const std::string& workdir);
    std::string list_jobs();
    std::string get_job(const std::string& job_id);
    std::string stop_job(const std::string& job_id,
                         const std::optional<std::int64_t>& call_timeout_millis);
    std::string acknowledge_job(const std::string& job_id);
    shell_client::ShellClient::FileImageResult view_image(const std::string& path);

private:
    std::string execute_command_internal(const std::string& command,
                                         const std::string& workdir, std::int32_t timeout_ms,
                                         const std::function<void(const std::string&)>& on_output);

    std::optional<ShellDeviceConfig> device_;
    std::string url_;
    std::string api_key_;
    std::string device_name_;
    shell_client::ShellClient client_;
    HttpStreamTransport& stream_transport_;
};

// ---------------------------------------------------------------------------------------------
// SshBackend
// ---------------------------------------------------------------------------------------------

class SshBackend final : public Backend {
public:
    SshBackend(ShellDeviceConfig device, SshTransport& transport);

    const std::optional<ShellDeviceConfig>& device() const override { return device_; }

    std::string execute_command(const std::string& command, const std::string& workdir,
                                std::int32_t timeout_ms) override;

    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit) override;
    std::optional<std::string> file_write(const std::string& path,
                                          const std::string& content) override;
    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& old_string,
                                              const std::string& new_string,
                                              bool replace_all) override;
    GlobOutcome file_glob(const std::string& pattern, const std::string& base_path,
                          const std::optional<std::int32_t>& depth) override;
    GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                          const std::string& file_glob) override;
    void close() override { client_.close(); }

private:
    std::optional<ShellDeviceConfig> device_;
    std::string host_;
    std::string device_name_;
    SshClient client_;
};

// ---------------------------------------------------------------------------------------------
// SandboxBackend
// ---------------------------------------------------------------------------------------------

class SandboxBackend final : public Backend {
public:
    /**
     * Kotlin `sandbox ?: throw IllegalStateException("Sandbox not available")`: a null manager
     * throws std::runtime_error with that message at construction.
     */
    explicit SandboxBackend(SandboxManager* manager);

    const std::optional<ShellDeviceConfig>& device() const override { return device_; }

    std::string execute_command(const std::string& command, const std::string& workdir,
                                std::int32_t timeout_ms) override;

    shell_tools::ShellFileReadResult file_read(const std::string& path, std::int64_t offset,
                                               std::int64_t limit) override;
    std::optional<std::string> file_write(const std::string& path,
                                          const std::string& content) override;
    shell_file::ShellFileEditResult file_edit(const std::string& path,
                                              const std::string& old_string,
                                              const std::string& new_string,
                                              bool replace_all) override;
    GlobOutcome file_glob(const std::string& pattern, const std::string& base_path,
                          const std::optional<std::int32_t>& depth) override;
    GrepOutcome file_grep(const std::string& pattern, const std::string& base_path,
                          const std::string& file_glob) override;
    void close() override {}

private:
    std::optional<ShellDeviceConfig> device_;
    SandboxManager& manager_;
};

}  // namespace agora::core::shell_backends
