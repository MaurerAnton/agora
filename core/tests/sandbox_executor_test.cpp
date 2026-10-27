// Unit tests for the bounded process runner (ProotSandboxManager.executeRaw mechanics).
#include <chrono>
#include <string>
#include <vector>

#include "agora/core/sandbox_executor.hpp"
#include "check.hpp"

namespace sandbox = agora::core::sandbox;

namespace {

constexpr std::size_t kCap = 1u << 20;  // SHELL_COMMAND_OUTPUT_MAX_BYTES

}  // namespace

TEST(sandbox_executor_echo_output_and_zero_exit) {
    const sandbox::ProcessResult result = sandbox::run_bounded_process(
        {"/bin/sh", "-c", "printf 'hello'"}, {}, 5000, kCap);
    CHECK(!result.timed_out);
    CHECK(!result.truncated);
    CHECK_EQ(0, result.exit_code);
    CHECK_EQ(std::string("hello"), result.output);
}

TEST(sandbox_executor_merges_stderr_into_output) {
    // Kotlin redirectErrorStream(true): stderr lands in the same stream as stdout.
    const sandbox::ProcessResult result = sandbox::run_bounded_process(
        {"/bin/sh", "-c", "printf 'out'; printf 'err' >&2"}, {}, 5000, kCap);
    CHECK_EQ(0, result.exit_code);
    CHECK_EQ(std::string("outerr"), result.output);
}

TEST(sandbox_executor_propagates_exit_code) {
    const sandbox::ProcessResult result =
        sandbox::run_bounded_process({"/bin/sh", "-c", "exit 7"}, {}, 5000, kCap);
    CHECK_EQ(7, result.exit_code);
    CHECK(!result.timed_out);
}

TEST(sandbox_executor_applies_env_overrides) {
    const sandbox::ProcessResult result = sandbox::run_bounded_process(
        {"/bin/sh", "-c", "printf '%s' \"$AGORA_TEST_VALUE\""},
        {"AGORA_TEST_VALUE=env-ok"}, 5000, kCap);
    CHECK_EQ(0, result.exit_code);
    CHECK_EQ(std::string("env-ok"), result.output);
}

TEST(sandbox_executor_timeout_kills_and_reports) {
    const auto started = std::chrono::steady_clock::now();
    const sandbox::ProcessResult result = sandbox::run_bounded_process(
        {"/bin/sh", "-c", "sleep 30"}, {}, 200, kCap);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    CHECK(result.timed_out);
    CHECK_EQ(-1, result.exit_code);
    // Kill + 250 ms drain grace; must not wait for the full sleep.
    CHECK(elapsed < 5000);
}

TEST(sandbox_executor_truncates_at_cap_utf8_complete) {
    // 4000 ASCII bytes followed by a 3-byte Euro sign; cap at 4002 keeps a partial code point.
    const std::string command =
        "printf '" + std::string(4000, 'x') + "'; printf '\\342\\202\\254'";
    const sandbox::ProcessResult result =
        sandbox::run_bounded_process({"/bin/sh", "-c", command}, {}, 5000, 4002);
    CHECK(result.truncated);
    // The retained prefix must be UTF-8 complete: the split Euro sign is dropped.
    CHECK_EQ(std::size_t(4000), result.output.size());
    CHECK(result.output.find('x') != std::string::npos);
}

TEST(sandbox_executor_missing_binary_throws) {
    bool threw = false;
    try {
        sandbox::run_bounded_process({"/nonexistent/agora-proot"}, {}, 1000, kCap);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(sandbox_executor_stdin_reading_command_times_out) {
    // ProcessBuilder never writes the child's stdin pipe, so `cat` blocks until the kill.
    const auto started = std::chrono::steady_clock::now();
    const sandbox::ProcessResult result =
        sandbox::run_bounded_process({"/bin/sh", "-c", "cat"}, {}, 300, kCap);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    CHECK(result.timed_out);
    CHECK(elapsed < 5000);
}