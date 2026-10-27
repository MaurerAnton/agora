#pragma once

// Concrete SshTransport over libssh2 (POSIX sockets). Compiled only when the host provides
// libssh2 (AGORA_CORE_HAVE_LIBSSH2, set by core/CMakeLists.txt after find_path/find_library):
// the Android build keeps JSch as its SSH stack, so this header stays dependency-free via a
// pimpl and disappears entirely when libssh2 is absent.

#ifdef AGORA_CORE_HAVE_LIBSSH2

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/shell_tool_backends.hpp"

namespace agora::core::ssh {

/**
 * Libssh2SshTransport: the JSch Session surface util/SshClient.kt drives, backed by libssh2
 * over a plain TCP socket. Mirrors the semantics the core expects:
 *
 *  - open_session/set_password/set_config only stage the session (JSch getSession + setters);
 *    connect(timeout, policy) performs TCP + KEX within the timeout, presents the server's
 *    host-key blob to the TOFU policy, then enforces StrictHostKeyChecking: "yes" accepts only
 *    Ok and throws JSch's exact messages for CHANGED/NOT_INCLUDED, "no" accepts any decision
 *    (capture mode), and finally authenticates in PreferredAuthentications order —
 *    "password" via libssh2_userauth_password, "publickey" via the IdentityFile config key
 *    (both are real JSch Session config keys; Kotlin's SshClient only sets the password path).
 *
 *  - exec: open_exec/set_command stage, connect_exec(timeout) opens the session channel and
 *    sends the command within the timeout; the exec_* accessors lazily drain stdout/stderr
 *    until EOF so SshClient's 100 ms completion poll doubles as the read loop. The session is
 *    used from one thread only (the SshClient contract), so no locking is needed.
 *
 *  - sftp: init/stat/mkdir/get/put/pwd/ls with JSch-shaped failures ("No such file: …",
 *    "No such directory: …"); get reopens the file on every call (ChannelSftp.get semantics).
 *
 * Every failure — TCP, KEX, auth, channel, SFTP, timeout — throws std::runtime_error, like the
 * rest of the transport boundary.
 */
class Libssh2SshTransport final : public shell_backends::SshTransport {
public:
    Libssh2SshTransport();
    ~Libssh2SshTransport() override;

    Libssh2SshTransport(const Libssh2SshTransport&) = delete;
    Libssh2SshTransport& operator=(const Libssh2SshTransport&) = delete;

    // ── Session ────────────────────────────────────────────────────────────────
    void open_session(const std::string& user, const std::string& host,
                      std::int32_t port) override;
    void set_password(const std::string& password) override;
    void set_config(const std::string& key, const std::string& value) override;
    void connect(std::int32_t timeout_ms, shell_backends::HostKeyPolicy& policy) override;
    bool is_connected() override;
    void disconnect_session() override;

    // ── Exec channel ───────────────────────────────────────────────────────────
    void open_exec() override;
    void set_command(const std::string& command) override;
    void connect_exec(std::int32_t timeout_ms) override;
    bool exec_is_closed() override;
    std::int32_t exec_exit_status() override;
    std::string exec_stdout() override;
    std::string exec_stderr() override;
    void disconnect_exec() override;

    // ── SFTP channel ───────────────────────────────────────────────────────────
    void open_sftp() override;
    void connect_sftp(std::int32_t timeout_ms) override;
    SftpAttrs stat(const std::string& path) override;
    void mkdir(const std::string& path) override;
    std::string get(const std::string& path) override;
    void put(const std::string& path, std::string_view bytes) override;
    std::string pwd() override;
    std::vector<LsEntry> ls(const std::string& dir) override;
    void disconnect_sftp() override;

    // ── Clock ──────────────────────────────────────────────────────────────────
    std::int64_t clock_millis() override;
    void sleep_millis(std::int64_t millis) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace agora::core::ssh

#endif  // AGORA_CORE_HAVE_LIBSSH2