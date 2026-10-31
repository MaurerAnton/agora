// C++ replacement for server/test_submission_messages.py: pins the receivers' wire semantics
// (status codes, python-json bodies, allowlist, rate limit, rotation) and message_for's
// validation rules against the Python reference behaviour.
#include "check.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <sqlite3.h>

#include "agora/core/json.hpp"
#include "agora/server/crash_receiver.hpp"
#include "agora/server/http_server.hpp"
#include "agora/server/json_format.hpp"
#include "agora/server/patch_nginx.hpp"
#include "agora/server/rating_receiver.hpp"
#include "agora/server/submission_messages.hpp"

namespace {

namespace server = agora::server;

using server::CrashConfig;
using server::CrashReceiver;
using server::HttpServer;
using server::HttpRequest;
using server::HttpResponse;
using server::RatingConfig;
using server::SubmissionMessage;

const char* kMessageConfig =
    R"({"org.example.mobile": {"id": "reply-1", "title": "Thank You", "body": "Received", "buttonText": "OK"}})";

struct TempDir {
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("agora-server-test-" + std::to_string(stamp) + "-" + std::to_string(++counter));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

/** AGORA_SUBMISSION_MESSAGES scope guard, the equivalent of unittest.mock.patch.dict. */
struct MessagesEnv {
    explicit MessagesEnv(const std::string& config_path) {
        if (config_path.empty()) {
            ::unsetenv("AGORA_SUBMISSION_MESSAGES");
        } else {
            ::setenv("AGORA_SUBMISSION_MESSAGES", config_path.c_str(), 1);
        }
    }
    ~MessagesEnv() { ::unsetenv("AGORA_SUBMISSION_MESSAGES"); }
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

enum class Cl {
    Numeric,
    Invalid,
    Missing,
};

HttpRequest make_request(const std::string& method, const std::string& target,
                         const std::string& body, Cl cl = Cl::Numeric,
                         std::optional<long long> numeric = std::nullopt,
                         std::shared_ptr<std::atomic<bool>> read_flag = nullptr) {
    HttpRequest request;
    request.method = method;
    request.target = target;
    switch (cl) {
        case Cl::Numeric:
            request.has_content_length = true;
            request.content_length = numeric.value_or(static_cast<long long>(body.size()));
            break;
        case Cl::Invalid:
            request.has_content_length = true;
            request.content_length_invalid = true;
            break;
        case Cl::Missing:
            break;
    }
    const std::string payload = body;
    request.read_body = [payload, read_flag](std::size_t max_bytes) {
        if (read_flag != nullptr) {
            read_flag->store(true);
        }
        return payload.substr(0, max_bytes);
    };
    return request;
}

struct WireResponse {
    int status = 0;
    std::string head;
    std::string body;
};

std::string wire_request(const std::string& method, const std::string& target,
                         const std::string& body) {
    std::string request = method + " " + target + " HTTP/1.0\r\nHost: 127.0.0.1\r\n";
    if (!body.empty()) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    return request;
}

WireResponse http_exchange(int port, const std::string& raw_request) {
    WireResponse response;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return response;
    }
    timeval receive_timeout{};
    receive_timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    std::size_t sent = 0;
    while (sent < raw_request.size()) {
        const ssize_t wrote = ::send(fd, raw_request.data() + sent, raw_request.size() - sent, 0);
        if (wrote <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    std::string received;
    char buffer[8192];
    for (;;) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            break;
        }
        received.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);

    const std::size_t head_end = received.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        return response;
    }
    response.head = received.substr(0, head_end + 4);
    response.body = received.substr(head_end + 4);
    const std::size_t space = response.head.find(' ');
    if (space != std::string::npos) {
        response.status = std::atoi(response.head.c_str() + space + 1);
    }
    return response;
}

std::vector<std::tuple<long long, std::string, std::string, std::string>> query_ratings(
    const std::filesystem::path& database) {
    std::vector<std::tuple<long long, std::string, std::string, std::string>> rows;
    sqlite3* handle = nullptr;
    if (sqlite3_open(database.c_str(), &handle) != SQLITE_OK) {
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return rows;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT rating, app, name, comment FROM ratings ORDER BY id",
                           -1, &statement, nullptr) == SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto text = [&](int column) {
                const unsigned char* value = sqlite3_column_text(statement, column);
                return std::string(value == nullptr ? "" : reinterpret_cast<const char*>(value));
            };
            rows.emplace_back(sqlite3_column_int64(statement, 0), text(1), text(2), text(3));
        }
        sqlite3_finalize(statement);
    }
    sqlite3_close(handle);
    return rows;
}

HttpResponse post_rating(const std::string& body, const RatingConfig& config) {
    return server::handle_rating_request(make_request("POST", "/api/rating", body), config);
}

}  // namespace

TEST(submission_message_exact_match_and_config_gates) {
    TempDir temp;
    const std::filesystem::path config = temp.path / "messages.json";
    write_file(config, kMessageConfig);
    const MessagesEnv env(config.string());

    const std::optional<SubmissionMessage> message = server::message_for("org.example.mobile");
    CHECK(message.has_value());
    CHECK_EQ(std::string("reply-1"), message->id);
    CHECK_EQ(std::string("Thank You"), message->title);
    CHECK_EQ(std::string("Received"), message->body);
    CHECK(message->button_text.has_value());
    CHECK_EQ(std::string("OK"), *message->button_text);

    // Exact match only: a longer dotted name is a different package.
    CHECK(!server::message_for("org.example.mobile.extra").has_value());
    CHECK(!server::message_for("").has_value());

    for (const std::string& content :
         std::vector<std::string>{"{}", "null", "invalid", "[1]",
                                  R"({"org.example.mobile": null})",
                                  R"({"org.example.mobile": {"id": 1, "title": "x", "body": "y"}})",
                                  std::string(1024 * 1024 + 1, ' ')}) {
        write_file(config, content);
        CHECK(!server::message_for("org.example.mobile").has_value());
    }

    std::filesystem::remove(config);
    CHECK(!server::message_for("org.example.mobile").has_value());
}

TEST(submission_message_field_limits_and_optional_button) {
    TempDir temp;
    const std::filesystem::path config = temp.path / "messages.json";
    const MessagesEnv env(config.string());

    auto config_with = [](const std::string& id, const std::string& title,
                          const std::string& body, const std::string& extra) {
        return R"({"org.example.mobile": {"id": ")" + id + R"(", "title": ")" + title +
               R"(", "body": ")" + body + "\"" + extra + "}}";
    };

    write_file(config, config_with(std::string(128, 'a'), "t", "b", ""));
    CHECK(server::message_for("org.example.mobile").has_value());
    write_file(config, config_with(std::string(129, 'a'), "t", "b", ""));
    CHECK(!server::message_for("org.example.mobile").has_value());
    write_file(config, config_with("i", std::string(200, 't'), "b", ""));
    CHECK(server::message_for("org.example.mobile").has_value());
    write_file(config, config_with("i", std::string(201, 't'), "b", ""));
    CHECK(!server::message_for("org.example.mobile").has_value());
    write_file(config, config_with("i", "t", std::string(8000, 'b'), ""));
    CHECK(server::message_for("org.example.mobile").has_value());
    write_file(config, config_with("i", "t", std::string(8001, 'b'), ""));
    CHECK(!server::message_for("org.example.mobile").has_value());

    // Blank-after-strip values fail; padded ones are stored stripped.
    write_file(config, R"({"org.example.mobile": {"id": "   ", "title": "t", "body": "b"}})");
    CHECK(!server::message_for("org.example.mobile").has_value());
    write_file(config,
               R"({"org.example.mobile": {"id": "  padded  ", "title": "t", "body": "b"}})");
    const std::optional<SubmissionMessage> padded = server::message_for("org.example.mobile");
    CHECK(padded.has_value());
    CHECK_EQ(std::string("padded"), padded->id);

    // buttonText is optional: missing, blank, non-string or too long -> dropped, not failed.
    write_file(config, config_with("i", "t", "b", ""));
    CHECK(!server::message_for("org.example.mobile")->button_text.has_value());
    write_file(config,
               config_with("i", "t", "b", R"(, "buttonText": ")" + std::string(80, 'k') + "\""));
    CHECK(server::message_for("org.example.mobile")->button_text.has_value());
    write_file(config,
               config_with("i", "t", "b", R"(, "buttonText": ")" + std::string(81, 'k') + "\""));
    CHECK(!server::message_for("org.example.mobile")->button_text.has_value());
    write_file(config,
               R"({"org.example.mobile": {"id": "i", "title": "t", "body": "b", "buttonText": ""}})");
    CHECK(!server::message_for("org.example.mobile")->button_text.has_value());
    write_file(config,
               R"({"org.example.mobile": {"id": "i", "title": "t", "body": "b", "buttonText": 7}})");
    CHECK(!server::message_for("org.example.mobile")->button_text.has_value());

    // No environment: the config file is never consulted.
    ::unsetenv("AGORA_SUBMISSION_MESSAGES");
    write_file(config, kMessageConfig);
    CHECK(!server::message_for("org.example.mobile").has_value());
}

TEST(rating_stores_submission_and_returns_optional_message) {
    TempDir temp;
    const std::filesystem::path config = temp.path / "messages.json";
    write_file(config, kMessageConfig);
    const MessagesEnv env(config.string());

    RatingConfig rating;
    rating.db_path = (temp.path / "ratings.db").string();

    const HttpResponse accepted =
        post_rating(R"({"rating": 4, "app": "org.example.mobile"})", rating);
    CHECK_EQ(200, accepted.status);
    CHECK_EQ(std::string("{\"ok\": true, \"message\": {\"id\": \"reply-1\", \"title\": "
                         "\"Thank You\", \"body\": \"Received\", \"buttonText\": \"OK\"}}"),
             accepted.body);

    const HttpResponse no_message =
        post_rating(R"({"rating": 3, "app": "org.example.other"})", rating);
    CHECK_EQ(200, no_message.status);
    CHECK_EQ(std::string("{\"ok\": true}"), no_message.body);

    const std::vector<std::tuple<long long, std::string, std::string, std::string>> rows =
        query_ratings(rating.db_path);
    CHECK_EQ(std::size_t(2), rows.size());
    CHECK_EQ(4LL, std::get<0>(rows[0]));
    CHECK_EQ(std::string("org.example.mobile"), std::get<1>(rows[0]));
    CHECK_EQ(3LL, std::get<0>(rows[1]));
}

TEST(rating_validation_chain_returns_python_error_bodies) {
    TempDir temp;
    RatingConfig rating;
    rating.db_path = (temp.path / "ratings.db").string();
    const MessagesEnv env("");

    const auto expect_error = [&rating](const std::string& body, int status,
                                        const std::string& message) {
        const HttpResponse response = post_rating(body, rating);
        CHECK_EQ(status, response.status);
        CHECK_EQ("{\"error\": \"" + message + "\"}", response.body);
        CHECK(response.cors);
        CHECK_EQ(std::string("application/json; charset=utf-8"), response.content_type);
    };

    expect_error(R"({"rating": 0, "app": "x"})", 400,
                 "rating must be an integer from 1 to 5");
    expect_error(R"({"rating": 6, "app": "x"})", 400,
                 "rating must be an integer from 1 to 5");
    expect_error(R"({"rating": true, "app": "x"})", 400,
                 "rating must be an integer from 1 to 5");
    expect_error(R"({"rating": 4.0, "app": "x"})", 400,
                 "rating must be an integer from 1 to 5");
    expect_error(R"({"rating": "4", "app": "x"})", 400,
                 "rating must be an integer from 1 to 5");
    expect_error(R"({"app": "x"})", 400, "rating must be an integer from 1 to 5");
    expect_error("{", 400, "invalid json");
    expect_error("[1, 2]", 400, "json object required");
    expect_error(std::string("{\"rating\": 4, \"app\": \"\xff\"}"), 400, "invalid json");

    // Content-Length semantics run before the body is read.
    const auto read_flag = std::make_shared<std::atomic<bool>>(false);
    const HttpResponse invalid_length = server::handle_rating_request(
        make_request("POST", "/api/rating", "{}", Cl::Invalid), rating);
    CHECK_EQ(400, invalid_length.status);
    CHECK_EQ(std::string(R"({"error": "invalid content length"})"), invalid_length.body);

    const HttpResponse zero_length = server::handle_rating_request(
        make_request("POST", "/api/rating", "", Cl::Numeric, 0, read_flag), rating);
    CHECK_EQ(400, zero_length.status);
    CHECK_EQ(std::string(R"({"error": "empty request"})"), zero_length.body);
    CHECK(!read_flag->load());

    const HttpResponse negative_length = server::handle_rating_request(
        make_request("POST", "/api/rating", "{}", Cl::Numeric, -5, read_flag), rating);
    CHECK_EQ(400, negative_length.status);
    CHECK_EQ(std::string(R"({"error": "empty request"})"), negative_length.body);

    const HttpResponse too_large = server::handle_rating_request(
        make_request("POST", "/api/rating", "{}", Cl::Numeric, 64 * 1024 + 1, read_flag), rating);
    CHECK_EQ(413, too_large.status);
    CHECK_EQ(std::string(R"({"error": "request too large"})"), too_large.body);
    CHECK(!read_flag->load());  // the cap runs before the body read, like python
}

TEST(rating_truncates_and_trims_text_fields_in_code_points) {
    TempDir temp;
    RatingConfig rating;
    rating.db_path = (temp.path / "ratings.db").string();
    const MessagesEnv env("");

    // 100 two-byte characters -> python [:64] keeps 64 code points, not 64 bytes.
    std::string app;
    app.reserve(200);
    for (int i = 0; i < 100; ++i) {
        app += "\xc3\xa9";  // é
    }
    const HttpResponse response =
        post_rating(R"({"rating": 5, "app": ")" + app + R"(", "name": "  Bob  "})", rating);
    CHECK_EQ(200, response.status);

    const auto rows = query_ratings(rating.db_path);
    CHECK_EQ(std::size_t(1), rows.size());
    CHECK_EQ(std::string("Bob"), std::get<2>(rows[0]));
    CHECK_EQ(std::size_t(64), agora::core::json::utf16_length(std::get<1>(rows[0])));
    CHECK_EQ(std::size_t(64), server::code_point_length(std::get<1>(rows[0])));

    // Non-string fields become "" and the comment cap counts code points.
    const std::string comment(10001, 'c');
    CHECK_EQ(200, post_rating(R"({"rating": 2, "app": 123, "comment": ")" + comment + "\"}",
                              rating)
                      .status);
    const auto after = query_ratings(rating.db_path);
    CHECK_EQ(std::size_t(2), after.size());
    CHECK_EQ(std::string(""), std::get<1>(after[1]));
    CHECK_EQ(std::size_t(10000), server::code_point_length(std::get<3>(after[1])));
}

TEST(rating_method_and_path_semantics) {
    TempDir temp;
    RatingConfig rating;
    rating.db_path = (temp.path / "ratings.db").string();
    const MessagesEnv env("");

    // GET answers 405 on any path (python's do_GET has no path check).
    const HttpResponse get =
        server::handle_rating_request(make_request("GET", "/whatever", ""), rating);
    CHECK_EQ(405, get.status);
    CHECK_EQ(std::string(R"({"error": "method not allowed"})"), get.body);

    // OPTIONS checks the path first.
    const HttpResponse options =
        server::handle_rating_request(make_request("OPTIONS", "/api/rating", ""), rating);
    CHECK_EQ(204, options.status);
    CHECK(!options.has_body);
    CHECK(options.cors);  // preflight carries Access-Control-* headers, like python
    const HttpResponse options_elsewhere =
        server::handle_rating_request(make_request("OPTIONS", "/api/other", ""), rating);
    CHECK_EQ(404, options_elsewhere.status);

    // POST: path checked before everything else; trailing slash normalizes away.
    const HttpResponse wrong_path =
        server::handle_rating_request(make_request("POST", "/api/other", "{}"), rating);
    CHECK_EQ(404, wrong_path.status);
    CHECK_EQ(std::string(R"({"error": "not found"})"), wrong_path.body);
    const HttpResponse trailing_slash = server::handle_rating_request(
        make_request("POST", "/api/rating/", R"({"rating": 1})"), rating);
    CHECK_EQ(200, trailing_slash.status);

    // Unknown methods are 501, like BaseHTTPRequestHandler.
    const HttpResponse put =
        server::handle_rating_request(make_request("PUT", "/api/rating", "{}"), rating);
    CHECK_EQ(501, put.status);
}

TEST(rating_over_http_round_trip_emits_python_headers) {
    TempDir temp;
    write_file(temp.path / "messages.json", kMessageConfig);
    const MessagesEnv env((temp.path / "messages.json").string());

    RatingConfig rating;
    rating.db_path = (temp.path / "ratings.db").string();
    rating.host = "127.0.0.1";
    rating.port = 0;

    HttpServer server_instance(rating.host, rating.port, "agora-rating/1.0",
                               [&rating](const HttpRequest& request) {
                                   return server::handle_rating_request(request, rating);
                               });
    server_instance.start();
    const int port = server_instance.bound_port();
    CHECK(port > 0);

    const WireResponse post = http_exchange(
        port, wire_request("POST", "/api/rating",
                           R"({"rating": 4, "app": "org.example.mobile"})"));
    CHECK_EQ(200, post.status);
    CHECK_EQ(std::string("{\"ok\": true, \"message\": {\"id\": \"reply-1\", \"title\": "
                         "\"Thank You\", \"body\": \"Received\", \"buttonText\": \"OK\"}}"),
             post.body);
    CHECK(post.head.find("Server: agora-rating/1.0") != std::string::npos);
    CHECK(post.head.find("Access-Control-Allow-Origin: *") != std::string::npos);
    CHECK(post.head.find("Content-Type: application/json; charset=utf-8") != std::string::npos);
    CHECK(post.head.find("Content-Length: ") != std::string::npos);
    CHECK(post.head.find("Connection: close") != std::string::npos);

    const WireResponse options = http_exchange(port, wire_request("OPTIONS", "/api/rating", ""));
    CHECK_EQ(204, options.status);
    CHECK(options.head.find("Access-Control-Allow-Methods: POST, OPTIONS") != std::string::npos);
    // No Content-Type of its own; "Allow-Headers: Content-Type" contains the bare substring.
    CHECK(options.head.find("Content-Type:") == std::string::npos);

    const WireResponse get = http_exchange(port, wire_request("GET", "/api/rating", ""));
    CHECK_EQ(405, get.status);

    server_instance.stop();
}

TEST(crash_sanitizes_records_and_answers_with_message) {
    TempDir temp;
    const std::filesystem::path config = temp.path / "messages.json";
    write_file(config, kMessageConfig);
    const MessagesEnv env(config.string());

    CrashConfig crash;
    crash.log_file = (temp.path / "crashes.jsonl").string();
    CrashReceiver receiver(crash);

    const HttpResponse response = receiver.handle(make_request(
        "POST", "/crash",
        R"({"trace": "example", "packageName": "org.example.mobile", )"
        R"("versionCode": 31, "ts": 12345, "unknown": "ignored", "nothing": null})"));
    CHECK_EQ(200, response.status);
    CHECK_EQ(std::string("{\"ok\": true, \"message\": {\"id\": \"reply-1\", \"title\": "
                         "\"Thank You\", \"body\": \"Received\", \"buttonText\": \"OK\"}}"),
             response.body);

    const std::string record_line = read_file(crash.log_file);
    CHECK(!record_line.empty());
    CHECK_EQ('\n', record_line.back());
    const agora::core::json::ParseResult parsed =
        agora::core::json::parse(record_line.substr(0, record_line.size() - 1));
    CHECK(parsed.value.has_value());
    CHECK(parsed.value->kind == agora::core::json::Kind::Object);
    const auto& members = parsed.value->members;

    // Allowlist order is the tuple order of the python reference.
    CHECK_EQ(std::size_t(5), members.size());
    CHECK_EQ(std::string("trace"), members[0].key);
    CHECK_EQ(std::string("packageName"), members[1].key);
    CHECK_EQ(std::string("versionCode"), members[2].key);
    CHECK_EQ(std::string("ts"), members[3].key);
    CHECK_EQ(std::string("received_at"), members[4].key);
    CHECK(members[3].value.is_literal("12345"));
    // Dropped: the unknown field and null.
    for (const auto& member : members) {
        CHECK(member.key != "unknown");
        CHECK(member.key != "nothing");
    }
}

TEST(crash_legacy_and_invalid_config_answer_204) {
    TempDir temp;
    const std::filesystem::path config = temp.path / "messages.json";
    const MessagesEnv env(config.string());

    CrashConfig crash;
    crash.log_file = (temp.path / "crashes.jsonl").string();
    CrashReceiver receiver(crash);

    const HttpResponse legacy =
        receiver.handle(make_request("POST", "/crash", R"({"trace": "legacy"})"));
    CHECK_EQ(204, legacy.status);
    CHECK(!legacy.has_body);

    write_file(config, "invalid");
    const HttpResponse broken = receiver.handle(make_request(
        "POST", "/crash", R"({"trace": "x", "packageName": "org.example.mobile"})"));
    CHECK_EQ(204, broken.status);

    // GET stays a bare health check.
    const HttpResponse health = receiver.handle(make_request("GET", "/anything", ""));
    CHECK_EQ(200, health.status);
    CHECK(!health.has_body);
}

TEST(crash_body_gates_and_rate_limit) {
    TempDir temp;
    const MessagesEnv env("");

    CrashConfig crash;
    crash.log_file = (temp.path / "crashes.jsonl").string();
    crash.rate_limit = 2;
    CrashReceiver receiver(crash);

    const auto read_flag = std::make_shared<std::atomic<bool>>(false);
    const HttpResponse invalid_length =
        receiver.handle(make_request("POST", "/crash", "{}", Cl::Invalid, std::nullopt, read_flag));
    CHECK_EQ(400, invalid_length.status);
    CHECK(!read_flag->load());

    const HttpResponse empty =
        receiver.handle(make_request("POST", "/crash", "", Cl::Numeric, 0, read_flag));
    CHECK_EQ(413, empty.status);  // python crash: empty length is 413, not 400
    CHECK(!read_flag->load());

    const HttpResponse too_large =
        receiver.handle(make_request("POST", "/crash", "{}", Cl::Numeric, 64 * 1024 + 1, read_flag));
    CHECK_EQ(413, too_large.status);
    CHECK(!read_flag->load());

    CHECK_EQ(204, receiver.handle(make_request("POST", "/crash", R"({"trace": "a"})")).status);
    CHECK_EQ(204, receiver.handle(make_request("POST", "/crash", R"({"trace": "b"})")).status);
    const HttpResponse throttled =
        receiver.handle(make_request("POST", "/crash", R"({"trace": "c"})"));
    CHECK_EQ(429, throttled.status);

    // The rate slot is consumed before parsing, so a malformed body is throttled too...
    CrashConfig other = crash;
    other.rate_limit = 1;
    other.log_file = (temp.path / "other.jsonl").string();
    CrashReceiver consume_first(other);
    CHECK_EQ(400, consume_first.handle(make_request("POST", "/crash", "{")).status);
    CHECK_EQ(429, consume_first.handle(make_request("POST", "/crash", "{")).status);

    // ... and plain malformed bodies answer 400.
    CrashConfig malformed = crash;
    malformed.rate_limit = 100;
    malformed.log_file = (temp.path / "malformed.jsonl").string();
    CrashReceiver lenient(malformed);
    CHECK_EQ(400, lenient.handle(make_request("POST", "/crash", "{")).status);
    CHECK_EQ(400, lenient.handle(make_request("POST", "/crash", "[1]")).status);
    CHECK_EQ(400,
             lenient.handle(make_request("POST", "/crash", std::string("{\"a\": \"\xff\"}"))).status);
}

TEST(crash_rotates_the_log_past_the_size_limit) {
    TempDir temp;
    const MessagesEnv env("");

    CrashConfig crash;
    crash.log_file = (temp.path / "crashes.jsonl").string();
    crash.max_log_bytes = 8;
    CrashReceiver receiver(crash);

    CHECK_EQ(204, receiver.handle(make_request("POST", "/crash", R"({"trace": "first"})")).status);
    CHECK_EQ(204, receiver.handle(make_request("POST", "/crash", R"({"trace": "second"})")).status);

    const std::filesystem::path rotated = temp.path / "crashes.jsonl.1";
    CHECK(std::filesystem::exists(rotated));
    CHECK(read_file(rotated).find("\"first\"") != std::string::npos);
    CHECK(read_file(crash.log_file).find("\"second\"") != std::string::npos);
    CHECK(read_file(crash.log_file).find("\"first\"") == std::string::npos);
}

TEST(patch_nginx_inserts_idempotently_and_needs_the_anchor) {
    const std::string site =
        "server {\n"
        "    listen 80;\n"
        "    location / {\n"
        "        root /var/www;\n"
        "    }\n"
        "}\n"
        "server {\n"
        "    listen 443;\n"
        "    location / {\n"
        "        root /var/www;\n"
        "    }\n"
        "}\n";

    const std::optional<std::string> patched = server::patch_nginx_text(site);
    CHECK(patched.has_value());
    CHECK_EQ(std::size_t(2), server::count_crash_locations(*patched));

    const auto count_of = [](const std::string& text, const std::string& needle) {
        std::size_t count = 0;
        std::size_t cursor = 0;
        while ((cursor = text.find(needle, cursor)) != std::string::npos) {
            ++count;
            cursor += needle.size();
        }
        return count;
    };
    // Every anchor survives and each block lands immediately before its anchor.
    CHECK_EQ(count_of(site, "    location / {"), count_of(*patched, "    location / {"));
    CHECK(patched->find("location = /crash {") < patched->find("    location / {"));

    // Idempotent: patching the patched text changes nothing.
    const std::optional<std::string> again = server::patch_nginx_text(*patched);
    CHECK(again.has_value());
    CHECK(*again == *patched);

    // Without the anchor the python tool exited 2; the function reports nullopt.
    CHECK(!server::patch_nginx_text("server {\n    listen 80;\n}\n").has_value());
}
