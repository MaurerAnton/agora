#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/shell_tools.hpp"

namespace agora::core::durable_jobs {

// The pure orchestration half of ShellDurableJobExecutor.kt: Conch durable-job terminal-state
// rules, the rolling output snapshot/cursor arithmetic, both wait/poll loops, the exact JSON
// every branch builds, and the terminal job acknowledgements. The transport itself stays a
// platform boundary behind DurableJobBackend (the real client owns OkHttp; the golden stand
// drives a scripted double), and the coroutine mechanics are reduced to explicit hooks:
// now/delay/is_active mirror System.currentTimeMillis, kotlinx delay and isActive.

using Device = shell_tools::ShellDeviceConfig;

/** Device list plus the tool-timeout budget (the GenerationContext fields the executor reads). */
struct JobToolContext {
    std::vector<Device> devices;
    std::int64_t tool_timeout_ms = 60'000;
};

/** terminalShellJobAcknowledgements output. */
struct JobAcknowledgement {
    std::string server_name;
    std::string job_id;
};

/** terminalShellJobAcknowledgements input (the ToolCallData fields it reads). */
struct ToolCallRecord {
    std::string tool_name;
    std::string arguments;
    std::string result;
};

/** conchJobOutputSnapshot. */
struct JobOutputSnapshot {
    std::string text;
    std::int64_t output_bytes = 0;
    bool truncated = false;
};

/** ConchJobOutputCursor.consume update. */
struct JobOutputUpdate {
    std::string delta;
    std::int64_t lost_bytes = 0;
};

/** A poll/transport failure (Kotlin `catch (e: Exception)` around backend calls). */
struct PollFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * Coroutine cancellation surfaced through the transport (Kotlin CancellationException).
 * waitForShellJobPolling rethrows it; execute_durable_foreground converts it into the
 * stop-the-job-then-propagate outcome.
 */
struct JobCancelled : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------------------------
// Pure helpers (member functions of the module on the Kotlin side)
// ---------------------------------------------------------------------------------------------

/** resolveConchDevice: non-ssh devices, name match (ignoreCase) or the single candidate. */
std::optional<Device> resolve_conch_device(std::string_view server_name,
                                           const std::vector<Device>& devices);

/** conchServerNotFoundMessage: the four-branch "which Conch server" message. */
std::string conch_server_not_found_message(std::string_view server_name,
                                           const std::vector<Device>& devices);

/**
 * ShellDurableJobExecutor.maxWaitMs: toolTimeoutMs - 5000 floored at 1000, narrowed to Int
 * with Kotlin's toInt() bit truncation (C++20 modular signed conversion).
 */
std::int32_t max_wait_ms(std::int64_t tool_timeout_ms);

/** isTerminalJobPayload: lifecycle `state` wins; only a state-less explicit `error` is terminal. */
bool is_terminal_job_payload(std::string_view raw);

/** conchJobOutputSnapshot: string output + non-negative integral output_bytes, truncated flag. */
std::optional<JobOutputSnapshot> conch_job_output_snapshot(std::string_view raw);

/**
 * ConchJobOutputCursor: turns the rolling {output, output_bytes} snapshots into an exactly-once
 * delta stream over the global byte cursor, reporting bytes evicted from the retention window.
 */
class ConchJobOutputCursor {
public:
    JobOutputUpdate consume(std::string_view raw);

private:
    std::int64_t emitted_bytes_ = 0;
};

/**
 * terminalShellJobAcknowledgements: only acknowledgeable shell tools whose committed result
 * carries a terminal state and a job id, deduplicated on (lowercased server, job id).
 */
std::vector<JobAcknowledgement> terminal_shell_job_acknowledgements(
    const std::vector<ToolCallRecord>& calls);

// ---------------------------------------------------------------------------------------------
// Transport boundary + coroutine hooks
// ---------------------------------------------------------------------------------------------

/** The Conch job/file transport: the real HTTP client or the stand's scripted double. */
class DurableJobBackend {
public:
    virtual ~DurableJobBackend() = default;

    /** The device this backend targets (JSON `server` fields and error messages use it). */
    virtual const std::string& device_name() const noexcept = 0;

    virtual std::string start_job(const std::string& command, const std::string& workdir,
                                  std::int32_t timeout_ms) = 0;
    virtual std::string get_job(const std::string& job_id) = 0;
    virtual std::string list_jobs() = 0;
    virtual std::string stop_job(const std::string& job_id,
                                 std::optional<std::int64_t> call_timeout_ms) = 0;
    virtual void close() = 0;
};

/** System.currentTimeMillis / kotlinx delay / CoroutineContext.isActive, injected for tests. */
struct DurableJobHooks {
    std::function<std::int64_t()> now;       // epoch millis
    std::function<void(std::int64_t)> delay; // suspends ~ms (cancellation-aware when cancelled)
    std::function<bool()> is_active;         // default: always active
};

// ---------------------------------------------------------------------------------------------
// waitForShellJobPolling — the shared wait loop (virtual clock in the stand)
// ---------------------------------------------------------------------------------------------

struct PollingParams {
    std::string job_id;
    std::string server_name;
    std::int32_t timeout_ms = 0;
    std::optional<std::int32_t> clamped_from;
    std::int32_t ceiling_ms = 0;
};

struct PollingHooks {
    std::function<std::int64_t()> now;
    std::function<void(std::int64_t)> delay;
    std::function<bool()> is_active;                          // default: always active
    std::function<std::string()> poll;                        // payload | PollFailure | JobCancelled
    std::function<void(const std::string&)> on_snapshot;      // published snapshot texts, in order
};

/** The wait loop itself: transient failures tolerated, five in a row are fatal. */
std::string wait_for_job_polling(const PollingParams& params, const PollingHooks& hooks);

// ---------------------------------------------------------------------------------------------
// Public executor entry points
// ---------------------------------------------------------------------------------------------

/**
 * waitForShellJob: validates job_id/timeout_ms, resolves the Conch device, clamps the request
 * against maxWaitMs (reporting the clamp in the timed-out note) and runs the wait loop.
 * Published snapshot texts are appended to `snapshots` as they are emitted (caller-owned, so a
 * cancellation mid-wait keeps the already-published texts, exactly like the Kotlin callback).
 */
std::string wait_for_job(std::string_view arguments, const JobToolContext& ctx,
                         const std::function<DurableJobBackend&(const Device&)>& make_backend,
                         const DurableJobHooks& hooks, std::vector<std::string>& snapshots);

struct DurableForegroundOutcome {
    std::string json;   // empty when cancelled
    bool cancelled = false;
    std::vector<std::string> outputs;  // cursor deltas and gap notices, in order
};

/**
 * executeDurableForeground: starts a durable job, then polls it as a bounded foreground wait.
 * A wait expiry returns ownership through job_id; a cancellation stops the remote job first.
 */
DurableForegroundOutcome execute_durable_foreground(
    std::string_view command, std::string_view workdir, std::int32_t wait_ms,
    DurableJobBackend& backend, const DurableJobHooks& hooks);

/** listShellJobs: passthrough of the Conch list payload or the exact jsonError. */
std::string list_shell_jobs(std::string_view arguments, const JobToolContext& ctx,
                            const std::function<DurableJobBackend&(const Device&)>& make_backend);

/** getShellJob: job_id validation first, then passthrough or jsonError; closes the backend. */
std::string get_shell_job(std::string_view arguments, const JobToolContext& ctx,
                          const std::function<DurableJobBackend&(const Device&)>& make_backend);

}  // namespace agora::core::durable_jobs
