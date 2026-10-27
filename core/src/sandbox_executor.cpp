#include "agora/core/sandbox_executor.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
// proot is a Linux platform boundary; the bounded runner has no Windows host path.
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agora::core::sandbox {

namespace {

constexpr long kProcessPollIntervalMs = 25;    // Kotlin PROCESS_POLL_INTERVAL_MS
constexpr long kOutputCloseGraceMs = 250;      // Kotlin PROCESS_OUTPUT_CLOSE_GRACE_MS

/** ShellClient.completeUtf8PrefixLength: never end the retained prefix mid-code-point. */
std::size_t complete_utf8_prefix_length(const std::string& bytes) {
    if (bytes.empty()) return 0;
    long long lead_index = static_cast<long long>(bytes.size()) - 1;
    while (lead_index >= 0 &&
           (static_cast<unsigned char>(bytes[lead_index]) & 0xC0) == 0x80) {
        --lead_index;
    }
    if (lead_index < 0) return 0;
    const auto lead = static_cast<unsigned char>(bytes[lead_index]);
    std::size_t expected = 1;
    if ((lead & 0x80) == 0x00) expected = 1;
    else if ((lead & 0xE0) == 0xC0) expected = 2;
    else if ((lead & 0xF0) == 0xE0) expected = 3;
    else if ((lead & 0xF8) == 0xF0) expected = 4;
    return (bytes.size() - static_cast<std::size_t>(lead_index) < expected)
               ? static_cast<std::size_t>(lead_index)
               : bytes.size();
}

long ms_until(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    const auto delta =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<long>(delta);
}

}  // namespace

ProcessResult run_bounded_process(const std::vector<std::string>& argv,
                                  const std::vector<std::string>& env,
                                  std::int32_t timeout_ms,
                                  std::size_t max_output_bytes) {
#if defined(_WIN32)
    (void)argv;
    (void)env;
    (void)timeout_ms;
    (void)max_output_bytes;
    throw std::runtime_error("run_bounded_process is POSIX-only (proot is a Linux boundary)");
#else
    if (argv.empty()) throw std::runtime_error("run_bounded_process: empty argv");
    if (access(argv[0].c_str(), X_OK) != 0) {
        // Kotlin: ProcessBuilder.start() throws IOException("Cannot run program …").
        throw std::runtime_error("cannot execute " + argv[0] + ": " + std::strerror(errno));
    }

    int out_pipe[2];
    if (pipe(out_pipe) != 0) throw std::runtime_error("pipe failed");
    // ProcessBuilder semantics: the child's stdin is a pipe the parent never writes, so a
    // stdin-reading command blocks until the timeout kills it.
    int in_pipe[2];
    if (pipe(in_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        throw std::runtime_error("pipe failed");
    }

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) raw_argv.push_back(const_cast<char*>(arg.c_str()));
    raw_argv.push_back(nullptr);

    std::vector<char*> raw_env;
    raw_env.reserve(env.size() + 1);
    for (const std::string& entry : env) raw_env.push_back(const_cast<char*>(entry.c_str()));
    raw_env.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(in_pipe[0]);
        close(in_pipe[1]);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        // Child: merge stderr into stdout (Kotlin redirectErrorStream(true)).
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        dup2(in_pipe[0], STDIN_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(in_pipe[0]);
        close(in_pipe[1]);
        for (const std::string& entry : env) {
            const auto eq = entry.find('=');
            if (eq == std::string::npos) continue;
            setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), 1);
        }
        execv(raw_argv[0], raw_argv.data());
        dprintf(STDERR_FILENO, "exec failed: %s", std::strerror(errno));
        _exit(127);
    }

    close(out_pipe[1]);
    close(in_pipe[0]);
    const int read_fd = out_pipe[0];
    const int stdin_writer = in_pipe[1];

    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto effective_timeout = std::max<std::int32_t>(timeout_ms, 1);  // Kotlin coerceAtLeast(1)
    const auto deadline = started + std::chrono::milliseconds(effective_timeout);
    std::chrono::steady_clock::time_point drain_deadline = deadline;
    bool killed = false;
    bool eof = false;

    while (!eof) {
        const auto now = std::chrono::steady_clock::now();
        if (!killed && now >= deadline) {
            // Kotlin destroys the process, then awaits the reader for a 250 ms grace.
            killed = true;
            result.timed_out = true;
            kill(pid, SIGKILL);
            drain_deadline = now + std::chrono::milliseconds(kOutputCloseGraceMs);
            continue;
        }
        if (killed && now >= drain_deadline) break;

        pollfd pfd{};
        pfd.fd = read_fd;
        pfd.events = POLLIN;
        const long wait_ms = killed
                                 ? ms_until(drain_deadline)
                                 : std::min(ms_until(deadline), kProcessPollIntervalMs);
        const int ready = poll(&pfd, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        char buffer[8192];
        const ssize_t count = read(read_fd, buffer, sizeof(buffer));
        if (count > 0) {
            const auto n = static_cast<std::size_t>(count);
            if (result.output.size() + n <= max_output_bytes) {
                result.output.append(buffer, n);
            } else {
                const std::size_t allowed = max_output_bytes - result.output.size();
                result.output.append(buffer, allowed);
                result.truncated = true;  // keep draining; the rest is discarded
            }
        } else if (count == 0) {
            eof = true;
        } else if (errno != EINTR) {
            break;
        }
    }

    close(read_fd);
    close(stdin_writer);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (result.truncated) {
        result.output.resize(complete_utf8_prefix_length(result.output));
    }
    if (result.timed_out) {
        result.exit_code = -1;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = -1;
    }
    return result;
#endif
}

}  // namespace agora::core::sandbox