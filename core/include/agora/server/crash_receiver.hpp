#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "agora/server/http_server.hpp"

namespace agora::server {

/** Configuration of the crash receiver; defaults match the Python service constants. */
struct CrashConfig {
    std::string host = "127.0.0.1";
    int port = 8092;
    std::string data_dir = "/var/lib/agora-crash";
    std::string log_file = "/var/lib/agora-crash/crashes.jsonl";
    long long max_body = 64 * 1024;                // mirrors nginx client_max_body_size
    std::size_t max_string = 64 * 1024;            // per-field cap, in code points
    long long max_log_bytes = 50LL * 1024 * 1024;  // rotate past this size
    int rate_limit = 60;                           // accepted reports per rolling window
    double rate_window_seconds = 60.0;
};

/**
 * Anonymous crash receiver: sanitizes to the allowlist, appends one JSONL record, rotates the
 * log past [CrashConfig::max_log_bytes], and answers with the optional submission message.
 * The rolling rate limit consumes a slot before the body is read (like the Python receiver),
 * so malformed bodies still count.
 */
class CrashReceiver {
  public:
    explicit CrashReceiver(CrashConfig config);

    HttpResponse handle(const HttpRequest& request);

  private:
    bool rate_ok();
    void rotate_if_needed();

    CrashConfig config_;
    std::mutex mutex_;
    std::vector<double> recent_;
};

}  // namespace agora::server
