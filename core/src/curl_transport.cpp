#include "agora/core/curl_transport.hpp"

#ifdef AGORA_CORE_HAVE_CURL

#if defined(_WIN32)

// libcurl + OpenSSL is a POSIX host transport; Android builds never configure
// AGORA_CORE_HAVE_CURL (no libcurl), so this branch only guards stray configs.

#else

#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "agora/core/curl_transport.hpp"
#include "agora/core/http_policy.hpp"
#include "agora/core/conch_network.hpp"
#include "agora/core/shell_tool_backends.hpp"
#include "agora/core/bounded_wire_read.hpp"

namespace agora::core {

namespace {

using namespace http_policy;
using Deadline = std::chrono::steady_clock::time_point;

void ensure_curl_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

Deadline deadline_from(std::int32_t timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(std::max<std::int32_t>(timeout_ms, 1));
}

Deadline deadline_from_ms(std::int64_t timeout_ms) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(std::max<std::int64_t>(timeout_ms, 1));
}

bool deadline_reached(const Deadline deadline) {
    return std::chrono::steady_clock::now() >= deadline;
}

struct CurlEasyDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};
using CurlEasyPtr = std::unique_ptr<CURL, CurlEasyDeleter>;

struct CurlMultiDeleter {
    void operator()(CURLM* handle) const { curl_multi_cleanup(handle); }
};
using CurlMultiPtr = std::unique_ptr<CURLM, CurlMultiDeleter>;

struct CurlSlistDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};
using CurlSlistPtr = std::unique_ptr<curl_slist, CurlSlistDeleter>;

CurlEasyPtr make_easy() {
    ensure_curl_global_init();
    CURL* raw = curl_easy_init();
    if (!raw) throw std::runtime_error("curl_easy_init failed");
    return CurlEasyPtr(raw);
}

curl_slist* append_headers(curl_slist* list, const std::vector<conch::Header>& headers) {
    for (const auto& h : headers) {
        if (h.name.empty() || h.value.empty()) continue;
        std::string line = h.name + ": " + h.value;
        list = curl_slist_append(list, line.c_str());
        if (!list) throw std::runtime_error("curl_slist_append failed");
    }
    return list;
}

curl_slist* build_curl_headers(const std::vector<conch::Header>& headers) {
    curl_slist* list = nullptr;
    for (const auto& h : headers) {
        if (h.name.empty() || h.value.empty()) continue;
        std::string line = h.name + ": " + h.value;
        list = curl_slist_append(list, line.c_str());
        if (!list) throw std::runtime_error("curl_slist_append failed");
    }
    return list;
}

std::size_t write_string_callback(char* ptr, std::size_t size, std::size_t nmemb,
                                  void* userdata) {
    const std::size_t bytes = size * nmemb;
    if (bytes == 0) return 0;
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, bytes);
    return bytes;
}

std::size_t write_header_callback(char* ptr, std::size_t size, std::size_t nmemb,
                                  void* userdata) {
    const std::size_t bytes = size * nmemb;
    if (bytes == 0) return 0;
    auto* headers = static_cast<std::vector<conch::Header>*>(userdata);
    std::string line(ptr, bytes);
    if (line.size() < 2) return bytes;
    const auto colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    const auto first = value.find_first_not_of(" \t");
    if (first != std::string::npos) value = value.substr(first);
    const auto last = value.find_last_not_of(" \t\r\n");
    if (last != std::string::npos) value.resize(last + 1);
    headers->push_back(conch::Header{std::move(name), std::move(value)});
    return bytes;
}

void apply_proxy(CURL* easy, const http_policy::ProxyDecision& proxy,
                 const std::optional<http_policy::ProxyConfig>& config) {
    if (proxy.kind == http_policy::ProxyKind::kDirect) {
        curl_easy_setopt(easy, CURLOPT_PROXY, "");
        return;
    }
    const std::string proxy_url = (proxy.kind == http_policy::ProxyKind::kSocks ? "socks5://" : "http://") +
                                  proxy.host + ":" + std::to_string(proxy.port);
    curl_easy_setopt(easy, CURLOPT_PROXY, proxy_url.c_str());
    if (proxy.kind == http_policy::ProxyKind::kSocks) {
        curl_easy_setopt(easy, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    } else {
        curl_easy_setopt(easy, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    }
    if (config.has_value() && !config->username.empty() &&
        config->type == http_policy::ProxyType::kHttp) {
        const std::string creds = config->username + ":" + config->password;
        curl_easy_setopt(easy, CURLOPT_PROXYUSERPWD, creds.c_str());
        curl_easy_setopt(easy, CURLOPT_PROXYAUTH, CURLAUTH_BASIC);
    }
}

void apply_timeout(CURL* easy, std::int64_t timeout_ms) {
    if (timeout_ms > 0) {
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    }
}

void apply_redirects(CURL* easy, bool follow_redirects, bool /*follow_ssl_redirects*/) {
    if (follow_redirects) {
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 20L);
    } else {
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    }
}

void apply_tls(CURL* easy) {
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// WireSource (impl of conch::RawResponse::Source via wire::Source)
// ---------------------------------------------------------------------------------------------

namespace wire {

class WireSource final : public Source {
public:
    explicit WireSource(std::string data) : data_(std::move(data)), pos_(0) {}

    bool request(std::int64_t byte_count) override {
        if (byte_count < 0) return false;
        return static_cast<std::int64_t>(data_.size() - pos_) >= byte_count;
    }

    std::string read_utf8() override {
        if (pos_ >= data_.size()) return "";
        std::string result = data_.substr(pos_);
        pos_ = data_.size();
        return result;
    }

    bool exhausted() override { return pos_ >= data_.size(); }

    std::string read_utf8_line_strict(std::int64_t limit) override {
        if (pos_ >= data_.size()) throw Eof{};
        if (limit <= 0) throw std::invalid_argument("limit must be positive");
        const std::size_t remaining = data_.size() - pos_;
        std::size_t eol = std::string::npos;
        for (std::size_t i = 0; i < remaining; ++i) {
            if (data_[pos_ + i] == '\n') { eol = i; break; }
            if (data_[pos_ + i] == '\r') {
                if (pos_ + i + 1 < data_.size() && data_[pos_ + i + 1] == '\n') {
                    eol = i + 2; break;
                }
                eol = i + 1; break;
            }
        }
        if (eol == std::string::npos) {
            if (remaining > static_cast<std::size_t>(limit)) throw Eof{};
            std::string line = data_.substr(pos_);
            pos_ = data_.size();
            return line;
        }
        if (eol + 1 > static_cast<std::size_t>(limit)) throw Eof{};
        std::string line = data_.substr(pos_, eol);
        pos_ += eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }

    std::int64_t buffer_size() override {
        return static_cast<std::int64_t>(data_.size() - pos_);
    }

private:
    std::string data_;
    std::size_t pos_ = 0;
};

}  // namespace wire

// ---------------------------------------------------------------------------------------------
// HttpStreamHandle (streaming SSE) - defined in the cpp to avoid header pollution
// ---------------------------------------------------------------------------------------------

struct CurlTransport::StreamHandle final : shell_backends::HttpStreamHandle {
    StreamHandle(CURLM* multi, const shell_backends::StreamCall& call,
                 const std::vector<conch::Header>& headers,
                 const Deadline deadline,
                 const std::optional<http_policy::ProxyConfig>& proxy_config)
        : multi_(multi), call_(call), deadline_(deadline) {
        easy_ = make_easy();
        curl_easy_setopt(easy_.get(), CURLOPT_URL, call.url.c_str());
        curl_easy_setopt(easy_.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(easy_.get(), CURLOPT_POSTFIELDS, call.json_body.c_str());
        curl_easy_setopt(easy_.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(call.json_body.size()));

        std::vector<conch::Header> stream_headers;
        stream_headers.reserve(call.headers.size() + 1);
        for (const auto& h : call.headers) {
            stream_headers.push_back(conch::Header{h.first, h.second});
        }
        stream_headers.push_back(conch::Header{"Content-Type", "application/json"});
        curl_easy_setopt(easy_.get(), CURLOPT_HTTPHEADER, build_curl_headers(stream_headers));
        curl_easy_setopt(easy_.get(), CURLOPT_HTTPHEADER, build_curl_headers(stream_headers));

        // Streaming: write function reads chunk by chunk into our buffer
        curl_easy_setopt(easy_.get(), CURLOPT_WRITEFUNCTION,
                         +[](char* ptr, std::size_t size, std::size_t nmemb,
                             void* userdata) -> std::size_t {
            const std::size_t bytes = size * nmemb;
            if (bytes == 0) return 0;
            auto* self = static_cast<StreamHandle*>(userdata);
            self->buffer_.append(ptr, bytes);
            return bytes;
        });
        curl_easy_setopt(easy_.get(), CURLOPT_WRITEDATA, this);

        curl_easy_setopt(easy_.get(), CURLOPT_HEADERFUNCTION,
                         +[](char* ptr, std::size_t size, std::size_t nmemb,
                             void* userdata) -> std::size_t {
            const std::size_t bytes = size * nmemb;
            if (bytes == 0) return 0;
            auto* self = static_cast<StreamHandle*>(userdata);
            self->header_buf_.append(ptr, bytes);
            return bytes;
        });
        curl_easy_setopt(easy_.get(), CURLOPT_HEADERDATA, this);

        curl_easy_setopt(easy_.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy_.get(), CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(easy_.get(), CURLOPT_TCP_KEEPIDLE, 30L);
        curl_easy_setopt(easy_.get(), CURLOPT_TCP_KEEPINTVL, 10L);

        apply_timeout(easy_.get(), 120'000);
        apply_tls(easy_.get());
        apply_redirects(easy_.get(), false, false);

        const auto decision = http_policy::resolve_proxy(call.url, std::nullopt);
        apply_proxy(easy_.get(), decision, std::nullopt);

        http_policy::guard_cleartext_credentials(call.url, call.headers);

        curl_multi_add_handle(multi_, easy_.get());
        performing_ = true;
    }

    ~StreamHandle() { close(); }

    std::int32_t code() const override {
        long http_code = 0;
        curl_easy_getinfo(easy_.get(), CURLINFO_RESPONSE_CODE, &http_code);
        return static_cast<std::int32_t>(http_code);
    }

    std::optional<std::string> error_body() override {
        if (!error_captured_) return std::nullopt;
        return error_buf_;
    }

    std::optional<std::string> read_line() override {
        if (closed_) return std::nullopt;
        // Drain any buffered complete lines first
        if (!lines_.empty()) {
            std::string line = lines_.front();
            lines_.pop_front();
            return line;
        }
        if (!performing_) return std::nullopt;

        int still_running = 0;
        CURLMcode mc = curl_multi_perform(multi_, &still_running);
        if (mc != CURLM_OK && mc != CURLM_CALL_MULTI_PERFORM) {
            throw std::runtime_error("curl_multi_perform: " +
                                     std::string(curl_multi_strerror(mc)));
        }
        if (still_running == 0) {
            int msgs_in_queue = 0;
            CURLMsg* msg = curl_multi_info_read(multi_, &msgs_in_queue);
            if (msg && msg->msg == CURLMSG_DONE) {
                performing_ = false;
                flush_buffer();
            }
            return lines_.empty() ? std::nullopt : std::optional<std::string>{lines_.front()};
        }

        int max_fd = -1;
        fd_set fd_read, fd_write, fd_except;
        FD_ZERO(&fd_read);
        FD_ZERO(&fd_write);
        FD_ZERO(&fd_except);
        curl_multi_fdset(multi_, &fd_read, &fd_write, &fd_except, &max_fd);
        if (max_fd >= 0) {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100'000; // 100 ms
            const int sel = select(max_fd + 1, &fd_read, &fd_write, &fd_except, &tv);
            (void)sel;
        }
        flush_buffer();
        return lines_.empty() ? std::nullopt : std::optional<std::string>{lines_.front()};
    }

    void close() override {
        if (closed_) return;
        closed_ = true;
        if (performing_) {
            curl_multi_remove_handle(multi_, easy_.get());
            performing_ = false;
        }
    }

private:
    void flush_buffer() {
        std::size_t pos = 0;
        while (pos < buffer_.size()) {
            const std::size_t eol = buffer_.find('\n', pos);
            if (eol == std::string::npos) break;
            std::string line = buffer_.substr(pos, eol - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines_.push_back(std::move(line));
            pos = eol + 1;
        }
        buffer_.erase(0, pos);
    }

    CURLM* multi_;
    CurlEasyPtr easy_;
    Deadline deadline_;
    shell_backends::StreamCall call_;
    bool performing_ = false;
    bool closed_ = false;
    std::string buffer_;
    std::string header_buf_;
    std::deque<std::string> lines_;
    std::string error_buf_;
    bool error_captured_ = false;
};

// ---------------------------------------------------------------------------------------------
// CurlTransport::Impl
// ---------------------------------------------------------------------------------------------

struct CurlTransport::Impl {
    CurlTransportConfig config;
    CurlMultiPtr multi;
    std::optional<http_policy::ProxyConfig> proxy_config;

    Impl(CurlTransportConfig cfg) : config(std::move(cfg)) {
        ensure_curl_global_init();
        multi = CurlMultiPtr(curl_multi_init());
        if (!multi) throw std::runtime_error("curl_multi_init failed");
    }

    ~Impl() = default;
};

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

CurlTransport::CurlTransport(CurlTransportConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
CurlTransport::~CurlTransport() = default;

// ── conch::Transport ───────────────────────────────────────────────────────────

conch::RawResponse CurlTransport::execute(const conch::Request& request) {
    CurlEasyPtr easy = make_easy();

    curl_easy_setopt(easy.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str());

    if (request.body) {
        curl_easy_setopt(easy.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDS, request.body->c_str());
        curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(request.body->size()));
    } else {
        curl_easy_setopt(easy.get(), CURLOPT_HTTPGET, 1L);
    }

    std::vector<conch::Header> headers = request.headers;
    if (!request.content_type.empty()) {
        bool has_ct = false;
        for (const auto& h : headers) {
            if (h.name == "Content-Type") { has_ct = true; break; }
        }
        if (!has_ct) headers.push_back({"Content-Type", request.content_type});
    }
    curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, build_curl_headers(headers));

    std::string body;
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, write_string_callback);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &body);

    std::vector<conch::Header> response_headers;
    curl_easy_setopt(easy.get(), CURLOPT_HEADERFUNCTION, write_header_callback);
    curl_easy_setopt(easy.get(), CURLOPT_HEADERDATA, &response_headers);

    apply_timeout(easy.get(), request.timeout_millis);
    apply_tls(easy.get());
    apply_redirects(easy.get(), impl_->config.follow_redirects, impl_->config.follow_ssl_redirects);
    apply_tls(easy.get());

    const auto proxy_decision = http_policy::resolve_proxy(request.url, impl_->proxy_config);
    apply_proxy(easy.get(), proxy_decision, impl_->proxy_config);

    std::vector<std::pair<std::string, std::string>> guard_headers;
    guard_headers.reserve(request.headers.size());
    for (const auto& h : request.headers) {
        guard_headers.emplace_back(h.name, h.value);
    }
    http_policy::guard_cleartext_credentials(request.url, guard_headers);

    CURLcode rc = curl_easy_perform(easy.get());
    long http_code = 0;
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &http_code);

    if (rc != CURLE_OK) {
        if (rc == CURLE_OPERATION_TIMEDOUT) {
            throw std::runtime_error("Request timed out");
        }
        if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
            rc == CURLE_COULDNT_RESOLVE_PROXY) {
            throw std::runtime_error("Connection failed");
        }
        if (rc == CURLE_SSL_CACERT || rc == CURLE_SSL_CERTPROBLEM ||
            rc == CURLE_SSL_CONNECT_ERROR) {
            throw std::runtime_error("TLS handshake failed");
        }
        throw std::runtime_error("curl: " + std::string(curl_easy_strerror(rc)));
    }

    conch::RawResponse response;
    response.code = static_cast<int>(http_code);
    response.headers = std::move(response_headers);
    response.body = std::make_unique<wire::WireSource>(std::move(body));
    return response;
}

// ── HttpStreamTransport ────────────────────────────────────────────────────────

std::unique_ptr<shell_backends::HttpStreamHandle>
CurlTransport::stream_post(const shell_backends::StreamCall& call) {
    const Deadline deadline = deadline_from_ms(call.max_line_bytes > 0 ? 120'000 : 60'000);
    const std::vector<conch::Header> headers(call.headers.begin(), call.headers.end());
    return std::make_unique<CurlTransport::StreamHandle>(impl_->multi.get(), call, headers, deadline,
                                          impl_->proxy_config);
}

}  // namespace agora::core

// ---------------------------------------------------------------------------------------------
// Proxy helpers (public, also used by HttpPolicy stand)
// ---------------------------------------------------------------------------------------------

namespace agora::core {

void install_curl_proxy(const std::optional<http_policy::ProxyConfig>& config) {
    // Process-wide proxy for the default curl easy handles (used by ConchTransport).
    // Called once at app startup by the Kotlin layer; here it's a no-op because
    // we apply proxy per-request via resolve_proxy/apply_proxy.
    (void)config;
}

void guard_curl_cleartext_credentials(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    http_policy::guard_cleartext_credentials(url, headers);
}

}  // namespace agora::core

#endif  // _WIN32

#endif  // AGORA_CORE_HAVE_CURL