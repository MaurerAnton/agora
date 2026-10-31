#include "agora/server/rating_receiver.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>

#include "agora/core/json.hpp"
#include "agora/server/json_format.hpp"
#include "agora/server/submission_messages.hpp"

namespace agora::server {
namespace json = ::agora::core::json;  // sibling namespace of agora::server

namespace {

/** The exact DDL the Python receiver executes on every connection. */
constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS ratings ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "rating INTEGER NOT NULL CHECK(rating >= 1 AND rating <= 5), "
    "app TEXT DEFAULT '', "
    "name TEXT DEFAULT '', "
    "email TEXT DEFAULT '', "
    "comment TEXT DEFAULT '', "
    "created_at TEXT NOT NULL)";

constexpr const char* kInsertSql =
    "INSERT INTO ratings (rating, app, name, email, comment, created_at) "
    "VALUES (?, ?, ?, ?, ?, ?)";

HttpResponse json_response(int status, std::string body, bool cors = true) {
    HttpResponse response;
    response.status = status;
    response.content_type = "application/json; charset=utf-8";
    response.cors = cors;
    response.has_body = true;
    response.body = std::move(body);
    return response;
}

HttpResponse error_response(int status, std::string_view message) {
    std::string body = "{\"error\": ";
    body += py_string(message);
    body += "}";
    return json_response(status, std::move(body));
}

HttpResponse empty_response(int status) {
    HttpResponse response;
    response.status = status;
    response.cors = true;  // python's _send_empty emits the CORS block (OPTIONS preflight)
    return response;
}

/** datetime.now(timezone.utc).isoformat(): microseconds only when they are non-zero. */
std::string utc_iso_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000000;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
    std::string out = buffer;
    if (micros != 0) {
        char fraction[16];
        std::snprintf(fraction, sizeof(fraction), ".%06lld",
                      static_cast<long long>(micros));
        out += fraction;
    }
    out += "+00:00";
    return out;
}

const json::Value* last_member(const json::Value& object, std::string_view key) {
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) {
            found = &member.value;
        }
    }
    return found;
}

/** Python: `value.strip()[:limit] if isinstance(value, str) else ""`. */
std::string python_text_field(const json::Value& object, std::string_view key,
                              std::size_t limit) {
    const json::Value* value = last_member(object, key);
    if (value == nullptr || value->kind != json::Kind::String) {
        return {};
    }
    return take_code_points(json::trim(value->text), limit);
}

bool store_rating(const RatingConfig& config, long long rating, const std::string& app,
                  const std::string& name, const std::string& email,
                  const std::string& comment) {
    sqlite3* database = nullptr;
    if (sqlite3_open(config.db_path.c_str(), &database) != SQLITE_OK) {
        if (database != nullptr) {
            sqlite3_close(database);
        }
        return false;
    }
    char* error = nullptr;
    if (sqlite3_exec(database, kSchemaSql, nullptr, nullptr, &error) != SQLITE_OK) {
        sqlite3_free(error);
        sqlite3_close(database);
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, kInsertSql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    const std::string created_at = utc_iso_timestamp();
    sqlite3_bind_int64(statement, 1, rating);
    sqlite3_bind_text(statement, 2, app.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, email.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, comment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 6, created_at.c_str(), -1, SQLITE_TRANSIENT);
    const bool stored = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return stored;
}

}  // namespace

RatingConfig rating_config_from_env() {
    RatingConfig config;
    if (const char* db = std::getenv("AGORA_RATING_DB"); db != nullptr && *db != '\0') {
        config.db_path = db;
    }
    if (const char* host = std::getenv("AGORA_RATING_HOST"); host != nullptr && *host != '\0') {
        config.host = host;
    }
    if (const char* port = std::getenv("AGORA_RATING_PORT"); port != nullptr && *port != '\0') {
        config.port = std::atoi(port);
    }
    return config;
}

HttpResponse handle_rating_request(const HttpRequest& request, const RatingConfig& config) {
    // Dispatch order mirrors BaseHTTPRequestHandler: GET answers on any path, the path check
    // lives inside do_OPTIONS/do_POST only, every other method is 501.
    if (request.method == "GET") {
        return error_response(405, "method not allowed");
    }
    const std::string path = request.path();
    if (request.method == "OPTIONS") {
        if (path != "/api/rating") {
            return error_response(404, "not found");
        }
        return empty_response(204);
    }
    if (request.method != "POST") {
        HttpResponse response;
        response.status = 501;
        response.content_type = "text/plain; charset=utf-8";
        response.has_body = true;
        response.body = "Unsupported method ('" + request.method + "')";
        return response;
    }
    if (path != "/api/rating") {
        return error_response(404, "not found");
    }

    if (request.content_length_invalid) {
        return error_response(400, "invalid content length");
    }
    const long long length = request.content_length.value_or(0);
    if (length <= 0) {
        return error_response(400, "empty request");
    }
    if (length > config.max_content_length) {
        return error_response(413, "request too large");
    }

    const std::string body = request.read_body(static_cast<std::size_t>(length));
    if (!valid_utf8(body)) {
        return error_response(400, "invalid json");
    }
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.value.has_value()) {
        return error_response(400, "invalid json");
    }
    if (parsed.value->kind != json::Kind::Object) {
        return error_response(400, "json object required");
    }

    const json::Value* rating_value = last_member(*parsed.value, "rating");
    long long rating = 0;
    bool rating_ok = rating_value != nullptr && rating_value->kind == json::Kind::Literal &&
                     is_json_integer(rating_value->text);
    if (rating_ok) {
        // Python ints are arbitrary precision; the range check decides anyway.
        rating = std::strtoll(rating_value->text.c_str(), nullptr, 10);
        rating_ok = rating >= 1 && rating <= 5;
    }
    if (!rating_ok) {
        return error_response(400, "rating must be an integer from 1 to 5");
    }

    const std::string app = python_text_field(*parsed.value, "app", 64);
    const std::string name = python_text_field(*parsed.value, "name", 100);
    const std::string email = python_text_field(*parsed.value, "email", 254);
    const std::string comment =
        python_text_field(*parsed.value, "comment", config.max_comment_length);

    if (!store_rating(config, rating, app, name, email, comment)) {
        return error_response(500, "storage failure");
    }

    std::string response_body = "{\"ok\": true";
    if (const std::optional<SubmissionMessage> message = message_for(app); message.has_value()) {
        response_body += ", \"message\": ";
        response_body += submission_message_json(*message);
    }
    response_body += "}";
    return json_response(200, std::move(response_body));
}

}  // namespace agora::server
