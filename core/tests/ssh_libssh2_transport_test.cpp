// Integration tests for Libssh2SshTransport against a throwaway localhost OpenSSH server.
//
// The fixture generates an ed25519 host key and a client keypair, then runs `sshd -D` as the
// current (unprivileged) user with pubkey auth only. Password auth cannot be exercised here
// (no writable /etc/shadow), so the Kotlin password path is covered as a failure case while
// the success paths run through JSch's publickey config keys — both are real
// `Session.setConfig` keys that the transport honours.
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <pwd.h>

#include "agora/core/ssh_libssh2_transport.hpp"
#include "check.hpp"

namespace ssh_transport = agora::core::ssh;
namespace backends = agora::core::shell_backends;

namespace {

namespace fs = std::filesystem;

// ── Throwaway sshd fixture ─────────────────────────────────────────────────────

struct SshdServer {
    fs::path dir;
    pid_t pid = -1;
    int port = 0;
    std::string user;
    std::string identity;
    bool started = false;

    ~SshdServer() { stop(); }

    void start() {
        if (started) return;
        std::error_code ec;
        std::random_device rd;
        dir = fs::temp_directory_path() /
              ("agora_sshd_" + std::to_string(rd() % 1000000));
        fs::create_directories(dir, ec);
        if (ec) throw std::runtime_error("cannot create sshd fixture dir: " + ec.message());

        const std::string keygen = AGORA_TEST_SSH_KEYGEN;
        const std::string host_key = (dir / "host_key").string();
        const std::string client_key = (dir / "client_key").string();
        if (std::system((keygen + " -q -t ed25519 -f '" + host_key + "' -N ''").c_str()) != 0 ||
            std::system((keygen + " -q -t ed25519 -f '" + client_key + "' -N ''").c_str()) != 0) {
            throw std::runtime_error("ssh-keygen failed");
        }
        identity = client_key;

        std::ifstream pub(client_key + ".pub");
        std::string pub_line;
        std::getline(pub, pub_line);
        pub.close();
        std::ofstream authorized(dir / "authorized_keys", std::ios::trunc);
        authorized << pub_line << "\n";
        authorized.close();

        const passwd* account = getpwuid(geteuid());
        if (account == nullptr || account->pw_name == nullptr) {
            throw std::runtime_error("cannot resolve current user");
        }
        user = account->pw_name;
        port = pick_free_port();
        if (port == 0) throw std::runtime_error("cannot allocate a local port");

        const std::string config_path = (dir / "sshd_config").string();
        std::ofstream config(config_path, std::ios::trunc);
        config << "Port " << port << "\n"
               << "ListenAddress 127.0.0.1\n"
               << "HostKey " << host_key << "\n"
               << "PidFile " << (dir / "sshd.pid").string() << "\n"
               << "AuthorizedKeysFile " << (dir / "authorized_keys").string() << "\n"
               << "StrictModes no\n"
               << "PubkeyAuthentication yes\n"
               << "PasswordAuthentication no\n"
               << "KbdInteractiveAuthentication no\n"
               << "UsePAM no\n"
               << "PermitRootLogin no\n"
               << "LogLevel ERROR\n"
               << "Subsystem sftp internal-sftp\n";
        config.close();

        const std::string log_path = (dir / "sshd.log").string();
        pid = ::fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            const std::string sshd = AGORA_TEST_SSHD;
            ::execl(sshd.c_str(), sshd.c_str(), "-f", config_path.c_str(), "-D", "-E",
                    log_path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        // Wait for the listener (or a startup failure surfaced in the log).
        const int deadline_ms = 8000;
        for (int waited = 0; waited < deadline_ms; waited += 50) {
            if (port_accepts(port)) {
                started = true;
                return;
            }
            int status = 0;
            const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
            if (reaped == pid) {
                pid = -1;
                throw std::runtime_error("sshd exited during startup: " + read_log(log_path));
            }
            ::usleep(50 * 1000);
        }
        stop();
        throw std::runtime_error("sshd never listened: " + read_log(log_path));
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        started = false;
    }

    static int pick_free_port() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            socklen_t length = sizeof(address);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
                port = ntohs(address.sin_port);
            }
        }
        ::close(fd);
        return port;
    }

    static bool port_accepts(int port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            rc = ::poll(&pfd, 1, 200);
            if (rc > 0) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
                rc = socket_error == 0 ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        ::close(fd);
        return rc == 0;
    }

    static std::string read_log(const std::string& path) {
        std::ifstream in(path);
        if (!in) return "(no sshd log)";
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return text.empty() ? "(empty sshd log)" : text;
    }
};

SshdServer& server() {
    static SshdServer instance;
    return instance;
}

/** JSch config surface for pubkey auth (Kotlin always sets password — covered as a failure). */
void configure_public_key_auth(ssh_transport::Libssh2SshTransport& transport) {
    transport.set_password("");
    transport.set_config("PreferredAuthentications", "publickey");
    transport.set_config("IdentityFile", server().identity);
}

void configure_connected(ssh_transport::Libssh2SshTransport& transport,
                         const std::string& strict = "yes") {
    transport.open_session(server().user, "127.0.0.1", server().port);
    configure_public_key_auth(transport);
    transport.set_config("StrictHostKeyChecking", strict);
}

struct FixedPolicy : backends::HostKeyPolicy {
    int decision;
    std::vector<std::uint8_t> seen;

    explicit FixedPolicy(int decision) : decision(decision) {}

    int check(const std::string&, const std::vector<std::uint8_t>& key) override {
        seen = key;
        return decision;
    }
};

/** SshClient's completion poll: poll exec_is_closed with a wall-clock deadline. */
bool wait_exec_closed(ssh_transport::Libssh2SshTransport& transport, std::int64_t timeout_ms) {
    const std::int64_t deadline = transport.clock_millis() + timeout_ms;
    while (!transport.exec_is_closed()) {
        if (transport.clock_millis() >= deadline) return false;
        transport.sleep_millis(50);
    }
    return true;
}

std::string captured_host_key_algorithm(const std::vector<std::uint8_t>& blob) {
    if (blob.size() <= 4) return "";
    const std::size_t length = (static_cast<std::size_t>(blob[0]) << 24) |
                               (static_cast<std::size_t>(blob[1]) << 16) |
                               (static_cast<std::size_t>(blob[2]) << 8) |
                               static_cast<std::size_t>(blob[3]);
    if (length == 0 || length + 4 > blob.size()) return "";
    return std::string(reinterpret_cast<const char*>(blob.data() + 4), length);
}

}  // namespace

// ── Connection ─────────────────────────────────────────────────────────────────

TEST(ssh_transport_connects_and_captures_ed25519_host_key) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, policy);
    CHECK(transport.is_connected());
    // The TOFU policy receives JSch's wire blob: 4-byte length + "ssh-ed25519" + key.
    CHECK_EQ(std::string("ssh-ed25519"), captured_host_key_algorithm(policy.seen));
    CHECK(policy.seen.size() > 40);
    transport.disconnect_session();
    CHECK(!transport.is_connected());
}

TEST(ssh_transport_reconnect_after_disconnect) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, policy);
    CHECK(transport.is_connected());
    transport.disconnect_session();

    // SshClient reconnects by staging a fresh session on the same transport.
    configure_connected(transport);
    FixedPolicy second(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, second);
    CHECK(transport.is_connected());
    transport.disconnect_session();
}

TEST(ssh_transport_strict_rejects_changed_key) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Changed);
    std::string message;
    try {
        transport.connect(10'000, policy);
    } catch (const std::exception& error) {
        message = error.what();
    }
    // JSch's exact exception text for CHANGED with StrictHostKeyChecking=yes.
    CHECK_EQ(std::string("Host key has been changed."), message);
    CHECK(!transport.is_connected());
}

TEST(ssh_transport_strict_rejects_unknown_key) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::NotIncluded);
    std::string message;
    try {
        transport.connect(10'000, policy);
    } catch (const std::exception& error) {
        message = error.what();
    }
    // JSch's exact exception text for NOT_INCLUDED with StrictHostKeyChecking=yes.
    CHECK_EQ(std::string("Host key for 127.0.0.1 not found"), message);
    CHECK(!transport.is_connected());
}

TEST(ssh_transport_capture_mode_accepts_any_key) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport, /*strict=*/"no");
    FixedPolicy policy(backends::HostKeyPolicy::NotIncluded);
    transport.connect(10'000, policy);
    CHECK(transport.is_connected());
    CHECK(!policy.seen.empty());
    transport.disconnect_session();
}

TEST(ssh_transport_password_auth_fails_against_pubkey_server) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    transport.open_session(server().user, "127.0.0.1", server().port);
    transport.set_password("wrong-password");
    transport.set_config("PreferredAuthentications", "password");
    transport.set_config("StrictHostKeyChecking", "yes");
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    std::string message;
    try {
        transport.connect(10'000, policy);
    } catch (const std::exception& error) {
        message = error.what();
    }
    // JSch surfaces a failed session connect as "Auth fail".
    CHECK(message.find("Auth fail") == 0);
    CHECK(!transport.is_connected());
}

TEST(ssh_transport_connect_refused_reports_errno) {
    server().start();
    const int closed_port = SshdServer::pick_free_port();
    ssh_transport::Libssh2SshTransport transport;
    transport.open_session(server().user, "127.0.0.1", closed_port);
    transport.set_config("StrictHostKeyChecking", "yes");
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    std::string message;
    try {
        transport.connect(3'000, policy);
    } catch (const std::exception& error) {
        message = error.what();
    }
    CHECK(message.find("Connection refused") != std::string::npos);
    CHECK(!transport.is_connected());
}

// ── Exec channel ───────────────────────────────────────────────────────────────

TEST(ssh_transport_exec_runs_command_and_collects_stdout) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, policy);

    transport.open_exec();
    transport.set_command("printf 'hello world'");
    transport.connect_exec(10'000);
    CHECK(wait_exec_closed(transport, 10'000));
    CHECK_EQ(0, transport.exec_exit_status());
    CHECK_EQ(std::string("hello world"), transport.exec_stdout());
    CHECK_EQ(std::string(""), transport.exec_stderr());
    transport.disconnect_exec();
    transport.disconnect_session();
}

TEST(ssh_transport_exec_reports_stderr_and_exit_code) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, policy);

    transport.open_exec();
    transport.set_command("echo out; echo err 1>&2; exit 7");
    transport.connect_exec(10'000);
    CHECK(wait_exec_closed(transport, 10'000));
    CHECK_EQ(7, transport.exec_exit_status());
    CHECK_EQ(std::string("out\n"), transport.exec_stdout());
    CHECK_EQ(std::string("err\n"), transport.exec_stderr());
    transport.disconnect_exec();
    transport.disconnect_session();
}

TEST(ssh_transport_exec_timeout_disconnects_and_channel_reuses) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, policy);

    // A non-terminating command must NOT pin the completion poll (SshClient's timeout path).
    transport.open_exec();
    transport.set_command("sleep 30");
    transport.connect_exec(10'000);
    CHECK(!wait_exec_closed(transport, 1'200));
    transport.disconnect_exec();

    // The same session accepts a fresh exec channel afterwards (JSch channel lifecycle).
    transport.open_exec();
    transport.set_command("printf 'second'");
    transport.connect_exec(10'000);
    CHECK(wait_exec_closed(transport, 10'000));
    CHECK_EQ(std::string("second"), transport.exec_stdout());
    transport.disconnect_exec();
    transport.disconnect_session();
}

// ── SFTP channel ───────────────────────────────────────────────────────────────

TEST(ssh_transport_sftp_round_trip) {
    server().start();
    ssh_transport::Libssh2SshTransport transport;
    configure_connected(transport);
    FixedPolicy policy(backends::HostKeyPolicy::Ok);
    transport.connect(10'000, policy);

    transport.open_sftp();
    transport.connect_sftp(10'000);

    const std::string home = transport.pwd();
    CHECK(!home.empty());
    CHECK(home.front() == '/');

    std::random_device rd;
    const std::string base = home + "/agora_sftp_" + std::to_string(rd() % 1000000);
    transport.mkdir(base);
    transport.put(base + "/file.txt", "hello sftp");

    const auto attrs = transport.stat(base + "/file.txt");
    CHECK(!attrs.is_dir);
    CHECK_EQ(10, static_cast<int>(attrs.size));  // "hello sftp"
    CHECK_EQ(std::string("hello sftp"), transport.get(base + "/file.txt"));

    transport.mkdir(base + "/sub");
    const auto entries = transport.ls(base);
    bool found_file = false;
    bool found_dir = false;
    for (const auto& entry : entries) {
        if (entry.filename == "file.txt") {
            found_file = true;
            CHECK(!entry.is_dir);
        }
        if (entry.filename == "sub") {
            found_dir = true;
            CHECK(entry.is_dir);
        }
    }
    CHECK(found_file);
    CHECK(found_dir);

    // JSch-shaped failures for missing paths.
    std::string stat_message;
    try {
        transport.stat(base + "/missing.txt");
    } catch (const std::exception& error) {
        stat_message = error.what();
    }
    CHECK(stat_message.find("No such file") != std::string::npos);

    std::string ls_message;
    try {
        transport.ls(base + "/no-such-dir");
    } catch (const std::exception& error) {
        ls_message = error.what();
    }
    CHECK(ls_message.find("No such directory") != std::string::npos);

    // mkdir on an existing directory fails best-effort style (SshClient swallows it).
    bool mkdir_threw = false;
    try {
        transport.mkdir(base);
    } catch (const std::exception&) {
        mkdir_threw = true;
    }
    CHECK(mkdir_threw);

    transport.disconnect_sftp();
    transport.disconnect_session();
}