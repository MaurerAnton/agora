#include "agora/core/ssh_libssh2_transport.hpp"

#ifdef AGORA_CORE_HAVE_LIBSSH2

#if defined(_WIN32)

// libssh2 is wired to POSIX sockets here; Windows builds never configure AGORA_CORE_HAVE_LIBSSH2
// (no libssh2.h is found), so this branch only guards a stray configuration.

#else

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <libssh2.h>
#include <libssh2_sftp.h>

namespace agora::core::ssh {

namespace {

using Deadline = std::chrono::steady_clock::time_point;
namespace backends = agora::core::shell_backends;

void ensure_libssh2_init() {
    static std::once_flag once;
    std::call_once(once, [] { libssh2_init(0); });
}

Deadline deadline_from(std::int32_t timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(std::max<std::int32_t>(timeout_ms, 1));
}

bool deadline_reached(const Deadline deadline) {
    return std::chrono::steady_clock::now() >= deadline;
}

/** Poll the session socket until the session wants to make progress or the deadline passes. */
bool wait_session(int sock, LIBSSH2_SESSION* session, const Deadline deadline) {
    if (deadline_reached(deadline)) return false;
    const int directions = libssh2_session_block_directions(session);
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = static_cast<short>(
        ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) ? POLLIN : 0) |
        ((directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0));
    if (pfd.events == 0) pfd.events = POLLIN | POLLOUT;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) return false;
    int ready = ::poll(&pfd, 1, static_cast<int>(remaining));
    while (ready < 0 && errno == EINTR) {
        const auto retry = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
        if (retry <= 0) return false;
        ready = ::poll(&pfd, 1, static_cast<int>(retry));
    }
    return ready > 0;
}

std::string session_error_text(LIBSSH2_SESSION* session, const char* what) {
    char* message = nullptr;
    int length = 0;
    const int code = libssh2_session_last_error(session, &message, &length, 0);
    std::string text = what;
    text += ": ";
    if (message != nullptr && length > 0) {
        text.append(message, static_cast<std::size_t>(length));
    } else {
        text += "libssh2 error ";
        text += std::to_string(code);
    }
    return text;
}

std::string sftp_error_text(LIBSSH2_SFTP* sftp, const std::string& what) {
    const unsigned long code = libssh2_sftp_last_error(sftp);
    return what + ": sftp error " + std::to_string(code);
}

const char* host_key_algorithm(int type) {
    switch (type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return "ssh-rsa";
        case LIBSSH2_HOSTKEY_TYPE_DSS: return "ssh-dss";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ecdsa-sha2-nistp256";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ecdsa-sha2-nistp384";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ecdsa-sha2-nistp521";
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ssh-ed25519";
        default: return "";
    }
}

bool looks_like_ssh_blob(const std::vector<std::uint8_t>& blob) {
    if (blob.size() <= 4) return false;
    const std::uint32_t declared = (static_cast<std::uint32_t>(blob[0]) << 24) |
                                   (static_cast<std::uint32_t>(blob[1]) << 16) |
                                   (static_cast<std::uint32_t>(blob[2]) << 8) |
                                   static_cast<std::uint32_t>(blob[3]);
    if (declared < 4 || declared > 32 || declared + 4ull > blob.size()) return false;
    for (std::uint32_t i = 0; i < declared; ++i) {
        const auto c = blob[4 + i];
        const bool algorithm_char = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                    (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!algorithm_char) return false;
    }
    return true;
}

/**
 * JSch's HostKeyRepository key is the wire blob: 4-byte length + algorithm name + key data.
 * libssh2 hands back the raw key plus a type enum, so the blob is assembled to match; a build
 * that already returns the prefixed form passes through.
 */
std::vector<std::uint8_t> to_jsch_host_key_blob(const char* key, std::size_t length, int type) {
    std::vector<std::uint8_t> blob(reinterpret_cast<const std::uint8_t*>(key),
                                   reinterpret_cast<const std::uint8_t*>(key) + length);
    if (looks_like_ssh_blob(blob)) return blob;
    const char* algorithm = host_key_algorithm(type);
    if (algorithm == nullptr || *algorithm == '\0') return blob;
    const auto algorithm_length = static_cast<std::uint32_t>(std::strlen(algorithm));
    std::vector<std::uint8_t> prefixed;
    prefixed.reserve(4 + algorithm_length + length);
    prefixed.push_back(static_cast<std::uint8_t>((algorithm_length >> 24) & 0xFF));
    prefixed.push_back(static_cast<std::uint8_t>((algorithm_length >> 16) & 0xFF));
    prefixed.push_back(static_cast<std::uint8_t>((algorithm_length >> 8) & 0xFF));
    prefixed.push_back(static_cast<std::uint8_t>(algorithm_length & 0xFF));
    prefixed.insert(prefixed.end(), algorithm, algorithm + algorithm_length);
    prefixed.insert(prefixed.end(), reinterpret_cast<const std::uint8_t*>(key),
                    reinterpret_cast<const std::uint8_t*>(key) + length);
    return prefixed;
}

std::vector<std::string> split_config_list(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string::npos ? value.size() : comma;
        std::string part = value.substr(start, end - start);
        const auto first = part.find_first_not_of(" \t");
        const auto last = part.find_last_not_of(" \t");
        if (first != std::string::npos) part = part.substr(first, last - first + 1);
        if (!part.empty()) parts.push_back(part);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return parts;
}

bool file_readable(const std::string& path) {
    return !path.empty() && ::access(path.c_str(), R_OK) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------------------------

struct Libssh2SshTransport::Impl {
    // Staged session (JSch getSession + setters; connect() performs the work).
    std::string user;
    std::string host;
    std::int32_t port = 22;
    std::string password;
    std::map<std::string, std::string> config;

    bool connected = false;
    int sock = -1;
    LIBSSH2_SESSION* session = nullptr;

    // Exec channel state; drained lazily by the exec_* accessors.
    bool exec_requested = false;
    std::string command;
    LIBSSH2_CHANNEL* exec_channel = nullptr;
    std::string exec_out;
    std::string exec_err;
    bool exec_closed = false;
    std::int32_t exec_status = 0;

    // SFTP channel state.
    bool sftp_requested = false;
    LIBSSH2_SFTP* sftp = nullptr;

    ~Impl() { free_session(); }

    std::string config_value(const std::string& key) const {
        const auto found = config.find(key);
        return found == config.end() ? "" : found->second;
    }

    /** Best-effort graceful close (JSch disconnect(): EOF + close), bounded and non-blocking. */
    void free_exec_channel() {
        if (exec_channel == nullptr) return;
        const Deadline grace = deadline_from(250);
        if (libssh2_channel_send_eof(exec_channel) == LIBSSH2_ERROR_EAGAIN) {
            while (session != nullptr && sock >= 0 && wait_session(sock, session, grace)) {
                if (libssh2_channel_send_eof(exec_channel) != LIBSSH2_ERROR_EAGAIN) break;
            }
        }
        libssh2_channel_close(exec_channel);
        libssh2_channel_free(exec_channel);
        exec_channel = nullptr;
    }

    void free_session() {
        if (sftp != nullptr) {
            libssh2_sftp_shutdown(sftp);
            sftp = nullptr;
        }
        if (exec_channel != nullptr) {
            libssh2_channel_free(exec_channel);
            exec_channel = nullptr;
        }
        if (session != nullptr) {
            libssh2_session_disconnect(session, "bye");
            libssh2_session_free(session);
            session = nullptr;
        }
        if (sock >= 0) {
            ::close(sock);
            sock = -1;
        }
        connected = false;
        exec_closed = false;
        exec_out.clear();
        exec_err.clear();
        exec_status = 0;
    }

    /** Open the session channel and send the staged command within the timeout. */
    void open_and_start_exec(const Deadline deadline) {
        if (!connected || session == nullptr) {
            throw std::runtime_error("SSH session is not connected");
        }
        if (exec_channel != nullptr) free_exec_channel();
        exec_closed = false;
        exec_status = 0;
        exec_out.clear();
        exec_err.clear();
        for (;;) {
            exec_channel = libssh2_channel_open_ex(session, "session", 7, 2 * 1024 * 1024, 32768,
                                                   nullptr, 0);
            if (exec_channel != nullptr) break;
            const int error = libssh2_session_last_errno(session);
            if (error == LIBSSH2_ERROR_EAGAIN && wait_session(sock, session, deadline)) continue;
            if (error == LIBSSH2_ERROR_EAGAIN) {
                throw std::runtime_error("exec channel open: timed out");
            }
            throw std::runtime_error(session_error_text(session, "exec channel open"));
        }
        for (;;) {
            const int rc = libssh2_channel_exec(exec_channel, command.c_str());
            if (rc == 0) break;
            if (rc == LIBSSH2_ERROR_EAGAIN && wait_session(sock, session, deadline)) continue;
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                free_exec_channel();
                throw std::runtime_error("exec start: timed out");
            }
            free_exec_channel();
            throw std::runtime_error(session_error_text(session, "exec start"));
        }
    }

    /** Read whatever is available on both streams (single-threaded, non-blocking session). */
    void drain_exec() {
        if (exec_channel == nullptr || exec_closed) return;
        char buffer[16384];
        for (const int stream : {0, 1}) {
            for (;;) {
                const ssize_t read =
                    libssh2_channel_read_ex(exec_channel, stream, buffer, sizeof(buffer));
                if (read > 0) {
                    if (stream == 0) {
                        exec_out.append(buffer, static_cast<std::size_t>(read));
                    } else {
                        exec_err.append(buffer, static_cast<std::size_t>(read));
                    }
                    continue;
                }
                break;  // EAGAIN (nothing buffered now), 0 = EOF, negative = stream error
            }
        }
        if (libssh2_channel_eof(exec_channel)) {
            // EOF only surfaces after both streams have delivered their last byte, so the
            // buffers above are complete — same ordering JSch guarantees before isClosed.
            exec_status = libssh2_channel_get_exit_status(exec_channel);
            exec_closed = true;
        }
    }

    void ensure_sftp(const Deadline deadline) {
        if (sftp != nullptr) return;
        if (!connected || session == nullptr) {
            throw std::runtime_error("SSH session is not connected");
        }
        for (;;) {
            sftp = libssh2_sftp_init(session);
            if (sftp != nullptr) break;
            const int error = libssh2_session_last_errno(session);
            if (error == LIBSSH2_ERROR_EAGAIN && wait_session(sock, session, deadline)) continue;
            if (error == LIBSSH2_ERROR_EAGAIN) throw std::runtime_error("sftp: timed out");
            throw std::runtime_error(session_error_text(session, "sftp open"));
        }
    }

    void require_sftp() const {
        if (sftp == nullptr) throw std::runtime_error("SFTP channel is not connected");
    }

    /** Run a session-level operation, waiting out EAGAIN until the deadline; returns its rc. */
    template <typename Operation>
    int await_eagain(const Operation& operation, const Deadline deadline, const char* what) {
        for (;;) {
            const int rc = operation();
            if (rc != LIBSSH2_ERROR_EAGAIN) return rc;
            if (!wait_session(sock, session, deadline)) {
                throw std::runtime_error(std::string(what) + ": timed out");
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Session surface
// ---------------------------------------------------------------------------------------------

Libssh2SshTransport::Libssh2SshTransport() : impl_(std::make_unique<Impl>()) {
    ensure_libssh2_init();
}

Libssh2SshTransport::~Libssh2SshTransport() = default;

void Libssh2SshTransport::open_session(const std::string& user, const std::string& host,
                                       std::int32_t port) {
    impl_->user = user;
    impl_->host = host;
    impl_->port = port;
}

void Libssh2SshTransport::set_password(const std::string& password) {
    impl_->password = password;
}

void Libssh2SshTransport::set_config(const std::string& key, const std::string& value) {
    impl_->config[key] = value;
}

bool Libssh2SshTransport::is_connected() { return impl_->connected; }

void Libssh2SshTransport::disconnect_session() { impl_->free_session(); }

void Libssh2SshTransport::connect(std::int32_t timeout_ms, backends::HostKeyPolicy& policy) {
    if (impl_->user.empty() || impl_->host.empty()) {
        throw std::runtime_error("SSH session is not configured");
    }
    // A fresh connect replaces any previous half-open session (JSch: a new Session per client).
    impl_->free_session();

    const Deadline deadline = deadline_from(timeout_ms);
    const std::string port_text = std::to_string(impl_->port);

    // ── TCP connect within the timeout ─────────────────────────────────────────
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int resolved = ::getaddrinfo(impl_->host.c_str(), port_text.c_str(), &hints, &addresses);
    if (resolved != 0 || addresses == nullptr) {
        throw std::runtime_error("connect: " + std::string(gai_strerror(resolved)));
    }
    std::string last_socket_error = "connect: no usable address";
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_socket_error = std::string("connect: ") + std::strerror(errno);
            continue;
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            int polled = remaining > 0 ? ::poll(&pfd, 1, static_cast<int>(remaining)) : 0;
            while (polled < 0 && errno == EINTR) {
                const auto retry = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
                polled = retry > 0 ? ::poll(&pfd, 1, static_cast<int>(retry)) : 0;
            }
            if (polled > 0) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
                if (socket_error == 0) {
                    rc = 0;
                } else {
                    errno = socket_error;
                    rc = -1;
                }
            } else {
                if (polled == 0) errno = ETIMEDOUT;
                rc = -1;
            }
        }
        if (rc == 0) {
            impl_->sock = fd;
            break;
        }
        last_socket_error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    if (impl_->sock < 0) throw std::runtime_error(last_socket_error);

    // ── SSH handshake ──────────────────────────────────────────────────────────
    impl_->session = libssh2_session_init_ex(nullptr, nullptr, nullptr, nullptr);
    if (impl_->session == nullptr) {
        impl_->free_session();
        throw std::runtime_error("handshake: session allocation failed");
    }
    libssh2_session_set_blocking(impl_->session, 0);
    const int handshake_rc = impl_->await_eagain(
        [&] { return libssh2_session_handshake(impl_->session, impl_->sock); }, deadline,
        "handshake");
    if (handshake_rc != 0) {
        const std::string message = session_error_text(impl_->session, "handshake");
        impl_->free_session();
        throw std::runtime_error(message);
    }

    // ── Host key → TOFU policy → StrictHostKeyChecking ─────────────────────────
    std::size_t key_length = 0;
    int key_type = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    const char* host_key =
        libssh2_session_hostkey(impl_->session, &key_length, &key_type);
    if (host_key == nullptr || key_length == 0) {
        impl_->free_session();
        throw std::runtime_error("handshake: no host key offered");
    }
    const std::vector<std::uint8_t> blob = to_jsch_host_key_blob(host_key, key_length, key_type);
    const int decision = policy.check(impl_->host, blob);
    const bool strict = impl_->config_value("StrictHostKeyChecking") == "yes";
    if (strict && decision != backends::HostKeyPolicy::Ok) {
        const std::string message = decision == backends::HostKeyPolicy::Changed
                                        ? "Host key has been changed."
                                        : "Host key for " + impl_->host + " not found";
        impl_->free_session();
        throw std::runtime_error(message);
    }

    // ── Authentication (PreferredAuthentications order, JSch config semantics) ──
    const std::string preferred = impl_->config_value("PreferredAuthentications").empty()
                                      ? "password"
                                      : impl_->config_value("PreferredAuthentications");
    bool authenticated = false;
    bool attempted = false;
    for (const std::string& method : split_config_list(preferred)) {
        if (method == "password") {
            attempted = true;
            for (;;) {
                const int rc = libssh2_userauth_password(impl_->session, impl_->user.c_str(),
                                                         impl_->password.c_str());
                if (rc == 0) {
                    authenticated = true;
                    break;
                }
                if (rc == LIBSSH2_ERROR_EAGAIN &&
                    wait_session(impl_->sock, impl_->session, deadline)) {
                    continue;
                }
                if (rc == LIBSSH2_ERROR_EAGAIN) {
                    impl_->free_session();
                    throw std::runtime_error("auth: timed out");
                }
                break;
            }
        } else if (method == "publickey") {
            const std::string identity = impl_->config_value("IdentityFile");
            if (!file_readable(identity)) continue;
            attempted = true;
            const std::string public_path = identity + ".pub";
            const char* public_key = file_readable(public_path) ? public_path.c_str() : nullptr;
            for (;;) {
                const int rc = libssh2_userauth_publickey_fromfile_ex(
                    impl_->session, impl_->user.c_str(),
                    static_cast<unsigned int>(impl_->user.size()), public_key, identity.c_str(),
                    nullptr);
                if (rc == 0) {
                    authenticated = true;
                    break;
                }
                if (rc == LIBSSH2_ERROR_EAGAIN &&
                    wait_session(impl_->sock, impl_->session, deadline)) {
                    continue;
                }
                if (rc == LIBSSH2_ERROR_EAGAIN) {
                    impl_->free_session();
                    throw std::runtime_error("auth: timed out");
                }
                break;
            }
        }
        if (authenticated) break;
    }
    if (!authenticated) {
        impl_->free_session();
        // JSch's message for a failed session connect ("Auth fail").
        throw std::runtime_error(attempted ? "Auth fail" : "Auth fail: no authentication method");
    }

    impl_->connected = true;
}

// ---------------------------------------------------------------------------------------------
// Exec channel
// ---------------------------------------------------------------------------------------------

void Libssh2SshTransport::open_exec() { impl_->exec_requested = true; }

void Libssh2SshTransport::set_command(const std::string& command) {
    impl_->command = command;
}

void Libssh2SshTransport::connect_exec(std::int32_t timeout_ms) {
    if (!impl_->exec_requested) throw std::runtime_error("exec channel is not open");
    impl_->open_and_start_exec(deadline_from(timeout_ms));
}

bool Libssh2SshTransport::exec_is_closed() {
    impl_->drain_exec();
    return impl_->exec_closed;
}

std::int32_t Libssh2SshTransport::exec_exit_status() {
    impl_->drain_exec();
    return impl_->exec_status;
}

std::string Libssh2SshTransport::exec_stdout() {
    impl_->drain_exec();
    return impl_->exec_out;
}

std::string Libssh2SshTransport::exec_stderr() {
    impl_->drain_exec();
    return impl_->exec_err;
}

void Libssh2SshTransport::disconnect_exec() {
    impl_->free_exec_channel();
    impl_->exec_requested = false;
    impl_->command.clear();
}

// ---------------------------------------------------------------------------------------------
// SFTP channel
// ---------------------------------------------------------------------------------------------

void Libssh2SshTransport::open_sftp() { impl_->sftp_requested = true; }

void Libssh2SshTransport::connect_sftp(std::int32_t timeout_ms) {
    if (!impl_->sftp_requested) throw std::runtime_error("sftp channel is not open");
    impl_->ensure_sftp(deadline_from(timeout_ms));
}

shell_backends::SshTransport::SftpAttrs Libssh2SshTransport::stat(const std::string& path) {
    impl_->require_sftp();
    const Deadline deadline = deadline_from(30'000);
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const int rc = impl_->await_eagain(
        [&] {
            return libssh2_sftp_stat_ex(impl_->sftp, path.c_str(),
                                        static_cast<unsigned int>(path.size()),
                                        LIBSSH2_SFTP_STAT, &attributes);
        },
        deadline, "stat");
    if (rc != 0) {
        const unsigned long code = libssh2_sftp_last_error(impl_->sftp);
        if (code == LIBSSH2_FX_NO_SUCH_FILE || code == LIBSSH2_FX_NO_SUCH_PATH) {
            // JSch's SftpException for a missing path ("No such file").
            throw std::runtime_error("No such file: " + path);
        }
        throw std::runtime_error(sftp_error_text(impl_->sftp, "stat of " + path));
    }
    SftpAttrs result;
    result.is_dir = LIBSSH2_SFTP_S_ISDIR(attributes.permissions) != 0;
    result.size = static_cast<std::int64_t>(attributes.filesize);
    return result;
}

void Libssh2SshTransport::mkdir(const std::string& path) {
    impl_->require_sftp();
    const Deadline deadline = deadline_from(30'000);
    int rc = LIBSSH2_ERROR_EAGAIN;
    while (rc == LIBSSH2_ERROR_EAGAIN) {
        rc = libssh2_sftp_mkdir_ex(impl_->sftp, path.c_str(),
                                   static_cast<unsigned int>(path.size()), 0755);
        if (rc == LIBSSH2_ERROR_EAGAIN && !wait_session(impl_->sock, impl_->session, deadline)) {
            throw std::runtime_error(sftp_error_text(impl_->sftp, "mkdir"));
        }
    }
    if (rc != 0) {
        const unsigned long code = libssh2_sftp_last_error(impl_->sftp);
        if (code == LIBSSH2_FX_PERMISSION_DENIED || code == LIBSSH2_FX_FAILURE ||
            code == LIBSSH2_FX_FILE_ALREADY_EXISTS) {
            throw std::runtime_error("Permission denied: " + path);
        }
        throw std::runtime_error(sftp_error_text(impl_->sftp, "mkdir of " + path));
    }
}

std::string Libssh2SshTransport::get(const std::string& path) {
    impl_->require_sftp();
    const Deadline deadline = deadline_from(30'000);
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    for (;;) {
        handle = libssh2_sftp_open_ex(impl_->sftp, path.c_str(),
                                      static_cast<unsigned int>(path.size()),
                                      LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
        if (handle != nullptr) break;
        const int error = libssh2_session_last_errno(impl_->session);
        if (error == LIBSSH2_ERROR_EAGAIN && wait_session(impl_->sock, impl_->session, deadline)) {
            continue;
        }
        if (error == LIBSSH2_ERROR_EAGAIN) {
            throw std::runtime_error(sftp_error_text(impl_->sftp, "open for read"));
        }
        const unsigned long code = libssh2_sftp_last_error(impl_->sftp);
        if (code == LIBSSH2_FX_NO_SUCH_FILE || code == LIBSSH2_FX_NO_SUCH_PATH) {
            throw std::runtime_error("No such file: " + path);
        }
        throw std::runtime_error(sftp_error_text(impl_->sftp, "open of " + path));
    }
    std::string out;
    char buffer[16384];
    for (;;) {
        const ssize_t read = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (read > 0) {
            out.append(buffer, static_cast<std::size_t>(read));
            continue;
        }
        if (read == LIBSSH2_ERROR_EAGAIN) {
            if (wait_session(impl_->sock, impl_->session, deadline)) continue;
            libssh2_sftp_close_handle(handle);
            throw std::runtime_error(sftp_error_text(impl_->sftp, "read of " + path));
        }
        if (read == 0) break;
        libssh2_sftp_close_handle(handle);
        throw std::runtime_error(sftp_error_text(impl_->sftp, "read of " + path));
    }
    libssh2_sftp_close_handle(handle);
    return out;
}

void Libssh2SshTransport::put(const std::string& path, std::string_view bytes) {
    impl_->require_sftp();
    const Deadline deadline = deadline_from(30'000);
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    for (;;) {
        handle = libssh2_sftp_open_ex(
            impl_->sftp, path.c_str(), static_cast<unsigned int>(path.size()),
            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644,
            LIBSSH2_SFTP_OPENFILE);
        if (handle != nullptr) break;
        const int error = libssh2_session_last_errno(impl_->session);
        if (error == LIBSSH2_ERROR_EAGAIN && wait_session(impl_->sock, impl_->session, deadline)) {
            continue;
        }
        if (error == LIBSSH2_ERROR_EAGAIN) {
            throw std::runtime_error(sftp_error_text(impl_->sftp, "open for write"));
        }
        throw std::runtime_error(sftp_error_text(impl_->sftp, "open for write of " + path));
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::size_t chunk = std::min<std::size_t>(bytes.size() - written, 16384);
        for (;;) {
            const ssize_t sent = libssh2_sftp_write(handle, bytes.data() + written, chunk);
            if (sent > 0) {
                written += static_cast<std::size_t>(sent);
                break;
            }
            if (sent == LIBSSH2_ERROR_EAGAIN &&
                wait_session(impl_->sock, impl_->session, deadline)) {
                continue;
            }
            libssh2_sftp_close_handle(handle);
            throw std::runtime_error(sftp_error_text(impl_->sftp, "write of " + path));
        }
    }
    libssh2_sftp_close_handle(handle);
}

std::string Libssh2SshTransport::pwd() {
    impl_->require_sftp();
    const Deadline deadline = deadline_from(30'000);
    char buffer[2048];
    for (;;) {
        const int rc = libssh2_sftp_symlink_ex(
            impl_->sftp, ".", 1, buffer, static_cast<unsigned int>(sizeof(buffer) - 1),
            LIBSSH2_SFTP_REALPATH);
        if (rc >= 0) return std::string(buffer, static_cast<std::size_t>(rc));
        if (rc == LIBSSH2_ERROR_EAGAIN && wait_session(impl_->sock, impl_->session, deadline)) {
            continue;
        }
        throw std::runtime_error(sftp_error_text(impl_->sftp, "realpath"));
    }
}

std::vector<shell_backends::SshTransport::LsEntry> Libssh2SshTransport::ls(
    const std::string& dir) {
    impl_->require_sftp();
    const Deadline deadline = deadline_from(30'000);
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    for (;;) {
        handle = libssh2_sftp_open_ex(impl_->sftp, dir.c_str(),
                                      static_cast<unsigned int>(dir.size()), 0, 0,
                                      LIBSSH2_SFTP_OPENDIR);
        if (handle != nullptr) break;
        const int error = libssh2_session_last_errno(impl_->session);
        if (error == LIBSSH2_ERROR_EAGAIN && wait_session(impl_->sock, impl_->session, deadline)) {
            continue;
        }
        if (error == LIBSSH2_ERROR_EAGAIN) {
            throw std::runtime_error(sftp_error_text(impl_->sftp, "opendir"));
        }
        const unsigned long code = libssh2_sftp_last_error(impl_->sftp);
        if (code == LIBSSH2_FX_NO_SUCH_FILE || code == LIBSSH2_FX_NO_SUCH_PATH) {
            throw std::runtime_error("No such directory: " + dir);
        }
        throw std::runtime_error(sftp_error_text(impl_->sftp, "opendir of " + dir));
    }
    std::vector<LsEntry> entries;
    char name_buffer[512];
    char long_buffer[1024];
    for (;;) {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const int rc = libssh2_sftp_readdir_ex(handle, name_buffer, sizeof(name_buffer),
                                               long_buffer, sizeof(long_buffer), &attributes);
        if (rc > 0) {
            std::string name(name_buffer, static_cast<std::size_t>(rc));
            if (name == "." || name == "..") continue;
            entries.push_back(LsEntry{name, LIBSSH2_SFTP_S_ISDIR(attributes.permissions) != 0});
            continue;
        }
        if (rc == 0) break;  // end of directory
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (wait_session(impl_->sock, impl_->session, deadline)) continue;
            libssh2_sftp_closedir(handle);
            throw std::runtime_error(sftp_error_text(impl_->sftp, "readdir of " + dir));
        }
        libssh2_sftp_closedir(handle);
        throw std::runtime_error(sftp_error_text(impl_->sftp, "readdir of " + dir));
    }
    libssh2_sftp_closedir(handle);
    return entries;
}

void Libssh2SshTransport::disconnect_sftp() {
    if (impl_->sftp != nullptr) {
        libssh2_sftp_shutdown(impl_->sftp);
        impl_->sftp = nullptr;
    }
    impl_->sftp_requested = false;
}

// ---------------------------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------------------------

std::int64_t Libssh2SshTransport::clock_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Libssh2SshTransport::sleep_millis(std::int64_t millis) {
    if (millis > 0) std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

}  // namespace agora::core::ssh

#endif  // _WIN32

#endif  // AGORA_CORE_HAVE_LIBSSH2