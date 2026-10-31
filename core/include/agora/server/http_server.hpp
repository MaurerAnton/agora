#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace agora::server {

/**
 * One parsed HTTP/1.x request. The body is NOT read during parsing: receivers decide how many
 * bytes they accept (size caps run before the read exactly like the Python handlers), so the
 * body arrives through the [read_body] callback, which blocks until it has read the requested
 * byte count or the peer closes.
 */
struct HttpRequest {
    std::string method;    // first request-line token, e.g. "POST"
    std::string target;    // request target exactly as sent (query string intact)
    std::vector<std::pair<std::string, std::string>> headers;
    bool has_content_length = false;
    std::optional<long long> content_length;  // set when the header parses as an integer
    bool content_length_invalid = false;      // header present but not an integer

    /** First header match, case-insensitive name, trimmed value; empty when absent. */
    std::string header(std::string_view name) const;

    /** Path component with query removed, trailing slashes stripped ("/" stays "/"). */
    std::string path() const;

    /** Reads up to [max_bytes] from the wire; empty string when the peer closed early. */
    std::function<std::string(std::size_t max_bytes)> read_body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type;  // empty -> no Content-Type header
    bool cors = false;         // emit the submission receiver's CORS block
    bool has_body = false;
    std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

/**
 * Minimal threaded HTTP/1.0 server: one thread per connection, Connection: close, no access
 * logging (the Python receivers kept the systemd journal quiet). `port = 0` binds an
 * ephemeral port for tests; the bound port is readable through bound_port().
 */
class HttpServer {
  public:
    HttpServer(std::string host, int port, std::string server_version, HttpHandler handler);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();
    int bound_port() const { return bound_port_; }
    void stop();

  private:
    void accept_loop();
    void serve(int client_fd);

    std::string host_;
    int port_;
    std::string server_version_;
    HttpHandler handler_;

    int listen_fd_ = -1;
    int bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
};

}  // namespace agora::server
