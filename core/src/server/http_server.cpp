#include "agora/server/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace agora::server {
namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr int kPollStepMillis = 200;
constexpr int kIoTimeoutSeconds = 10;

std::string ascii_trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string_view reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Status";
    }
}

std::string http_date() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[64];
    // IMF-fixdate, the shape BaseHTTPRequestHandler.send_header("Date", ...) emits.
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return buffer;
}

bool send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

struct ParsedHead {
    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct HeadBuffer {
    std::string head;      // bytes up to and including the header terminator
    std::string leftover;  // body bytes that arrived in the same segment
};

/** Reads until CRLFCRLF (or LFLF, like http.server's readline tolerance); nullopt on overflow/EOF. */
std::optional<HeadBuffer> read_head(int fd) {
    std::string buffer;
    char chunk[4096];
    for (;;) {
        const std::size_t end = buffer.find("\r\n\r\n");
        const std::size_t alt = buffer.find("\n\n");
        std::size_t consumed = 0;
        if (end != std::string::npos) {
            consumed = end + 4;
        } else if (alt != std::string::npos) {
            consumed = alt + 2;
        } else if (buffer.size() > kMaxHeaderBytes) {
            return std::nullopt;
        } else {
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) {
                return std::nullopt;
            }
            buffer.append(chunk, static_cast<std::size_t>(got));
            continue;
        }
        // Keep everything past the terminator: curl and the test client send the head and the
        // body in one segment, and dropping it would strand the body until the read timeout.
        HeadBuffer out;
        out.head = buffer.substr(0, consumed);
        out.leftover = buffer.substr(consumed);
        return out;
    }
}

std::optional<ParsedHead> parse_head(const std::string& head) {
    const std::size_t first_line_end = head.find('\n');
    if (first_line_end == std::string::npos) {
        return std::nullopt;
    }
    std::string first_line = head.substr(0, first_line_end);
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }
    const std::size_t method_end = first_line.find(' ');
    const std::size_t target_start = method_end == std::string::npos ? std::string::npos
                                                                     : method_end + 1;
    const std::size_t target_end =
        target_start == std::string::npos ? std::string::npos : first_line.find(' ', target_start);
    if (method_end == std::string::npos || target_end == std::string::npos) {
        return std::nullopt;
    }

    ParsedHead parsed;
    parsed.method = first_line.substr(0, method_end);
    parsed.target = first_line.substr(target_start, target_end - target_start);

    std::size_t cursor = first_line_end + 1;
    while (cursor < head.size()) {
        const std::size_t line_end = head.find('\n', cursor);
        if (line_end == std::string::npos) {
            break;
        }
        std::string line = head.substr(cursor, line_end - cursor);
        cursor = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;  // end of headers
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;  // http.server skips malformed lines silently
        }
        const std::string name = ascii_trim(line.substr(0, colon));
        if (name.empty()) {
            continue;
        }
        std::string lowered = name;
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        parsed.headers.emplace_back(std::move(lowered), ascii_trim(line.substr(colon + 1)));
    }
    return parsed;
}

/** Python int() on a plain integer header value: optional sign + digits (no underscores). */
bool parse_content_length(const std::string& raw, long long& out) {
    if (raw.empty()) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (raw[index] == '+' || raw[index] == '-') {
        negative = raw[index] == '-';
        ++index;
    }
    if (index >= raw.size()) {
        return false;
    }
    long long value = 0;
    for (; index < raw.size(); ++index) {
        const char digit = raw[index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = value * 10 + (digit - '0');
        if (value > 4611686018427387904LL) {
            return false;  // absurdly large: python would keep arbitrary precision; nobody means it
        }
    }
    out = negative ? -value : value;
    return true;
}

}  // namespace

std::string HttpRequest::header(std::string_view name) const {
    std::string lowered(name);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const auto& entry : headers) {
        if (entry.first == lowered) {
            return entry.second;
        }
    }
    return {};
}

std::string HttpRequest::path() const {
    std::string path = target.substr(0, target.find('?'));
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    if (path.empty()) {
        path = "/";
    }
    return path;
}

HttpServer::HttpServer(std::string host, int port, std::string server_version,
                       HttpHandler handler)
    : host_(std::move(host)), port_(port), server_version_(std::move(server_version)),
      handler_(std::move(handler)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("unsupported bind address: " + host_);
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string why = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("bind " + host_ + ":" + std::to_string(port_) + ": " + why);
    }
    if (::listen(listen_fd_, 64) != 0) {
        const std::string why = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("listen: " + why);
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }

    running_ = true;
    accept_thread_ = std::thread(&HttpServer::accept_loop, this);
}

void HttpServer::accept_loop() {
    while (running_) {
        pollfd descriptor{listen_fd_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, kPollStepMillis);
        if (!running_) {
            break;
        }
        if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!running_) {
                break;
            }
            continue;
        }
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back([this, client] { serve(client); });
    }
}

void HttpServer::serve(int client_fd) {
    timeval timeout{};
    timeout.tv_sec = kIoTimeoutSeconds;
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const std::optional<HeadBuffer> read = read_head(client_fd);
    const std::optional<ParsedHead> parsed =
        read.has_value() ? parse_head(read->head) : std::nullopt;
    if (!parsed.has_value()) {
        // python's http.server answers a malformed request line with a 400; keep it bare.
        send_all(client_fd, "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        ::close(client_fd);
        return;
    }

    HttpRequest request;
    request.method = parsed->method;
    request.target = parsed->target;
    request.headers = parsed->headers;
    for (const auto& entry : request.headers) {
        if (entry.first != "content-length") {
            continue;
        }
        request.has_content_length = true;
        long long value = 0;
        if (parse_content_length(entry.second, value)) {
            request.content_length = value;
        } else {
            request.content_length_invalid = true;
        }
        break;
    }
    const auto pending =
        std::make_shared<std::string>(read.has_value() ? std::move(read->leftover) : std::string());
    request.read_body = [client_fd, pending](std::size_t max_bytes) {
        std::string body;
        body.reserve(max_bytes);
        if (!pending->empty()) {
            const std::size_t take = std::min(max_bytes, pending->size());
            body.append(pending->data(), take);
            pending->erase(0, take);
        }
        char buffer[8192];
        while (body.size() < max_bytes) {
            const std::size_t want =
                std::min(sizeof(buffer), max_bytes - body.size());
            const ssize_t got = ::recv(client_fd, buffer, want, 0);
            if (got <= 0) {
                break;
            }
            body.append(buffer, static_cast<std::size_t>(got));
        }
        return body;
    };

    HttpResponse response;
    try {
        response = handler_(request);
    } catch (...) {
        // python leaked a traceback and closed the socket; a bare 500 is strictly better.
        response = HttpResponse{};
        response.status = 500;
        response.has_body = false;
    }

    std::string out;
    out.reserve(response.body.size() + 256);
    out += "HTTP/1.0 ";
    out += std::to_string(response.status);
    out += ' ';
    out += reason_phrase(response.status);
    out += "\r\nServer: ";
    out += server_version_;
    out += "\r\nDate: ";
    out += http_date();
    out += "\r\n";
    if (response.cors) {
        out += "Access-Control-Allow-Origin: *\r\n";
        out += "Access-Control-Allow-Methods: POST, OPTIONS\r\n";
        out += "Access-Control-Allow-Headers: Content-Type\r\n";
    }
    if (!response.content_type.empty()) {
        out += "Content-Type: ";
        out += response.content_type;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(response.has_body ? response.body.size() : 0);
    out += "\r\nConnection: close\r\n\r\n";
    if (response.has_body) {
        out += response.body;
    }
    send_all(client_fd, out);
    ::close(client_fd);
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

}  // namespace agora::server
