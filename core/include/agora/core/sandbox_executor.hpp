#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agora::core::sandbox {

/** Result of a bounded child process run (Kotlin executeRaw mechanics). */
struct ProcessResult {
    /** Captured stdout+stderr (Kotlin `redirectErrorStream(true)`), UTF-8 complete on truncation. */
    std::string output;
    /** Wait status: WEXITSTATUS, 128+signal when signalled, -1 when timed out. */
    std::int32_t exit_code = -1;
    /** The wall-clock timeout fired; the child was SIGKILLed. */
    bool timed_out = false;
    /** Output exceeded the cap; [output] holds the retained UTF-8 complete prefix. */
    bool truncated = false;
};

/**
 * run_bounded_process: fork/exec [argv] with [env] entries applied over the inherited
 * environment, read stdout+stderr through one pipe up to [max_output_bytes], and enforce
 * [timeout_ms] with a 25 ms poll slice (Kotlin's PROCESS_POLL_INTERVAL_MS) plus a 250 ms
 * drain grace after the kill (PROCESS_OUTPUT_CLOSE_GRACE_MS).
 *
 * On timeout the child is SIGKILLed, output collected so far is kept, exit_code is -1 and
 * timed_out is set — mirroring ProotSandboxManager.executeRaw's timeout path.
 *
 * Throws std::runtime_error only when the process cannot be created.
 */
ProcessResult run_bounded_process(const std::vector<std::string>& argv,
                                  const std::vector<std::string>& env,
                                  std::int32_t timeout_ms,
                                  std::size_t max_output_bytes);

}  // namespace agora::core::sandbox