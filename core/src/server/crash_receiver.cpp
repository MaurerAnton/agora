#include "agora/server/crash_receiver.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "agora/core/json.hpp"
#include "agora/server/json_format.hpp"
#include "agora/server/submission_messages.hpp"

namespace agora::server {
namespace json = ::agora::core::json;  // sibling namespace of agora::server

namespace {

/** Field order of the Python ALLOWED_FIELDS tuple: the record keeps exactly this shape. */
constexpr const char* kAllowedFields[] = {
    "trace",       "packageName", "appVersion", "versionCode",
    "androidApi",  "androidRelease", "device",   "ts",
};

HttpResponse bare_response(int status) {
    HttpResponse response;
    response.status = status;
    return response;
}

HttpResponse message_response(const SubmissionMessage& message) {
    std::string body = "{\"ok\": true, \"message\": ";
    body += submission_message_json(message);
    body += "}";
    HttpResponse response;
    response.status = 200;
    response.content_type = "application/json; charset=utf-8";
    response.has_body = true;
    response.body = std::move(body);
    return response;
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

}  // namespace

CrashReceiver::CrashReceiver(CrashConfig config) : config_(std::move(config)) {}

bool CrashReceiver::rate_ok() {
    const auto now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const double cutoff = now - config_.rate_window_seconds;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!recent_.empty() && recent_.front() < cutoff) {
        recent_.erase(recent_.begin());
    }
    if (static_cast<int>(recent_.size()) >= config_.rate_limit) {
        return false;
    }
    recent_.push_back(now);
    return true;
}

void CrashReceiver::rotate_if_needed() {
    std::error_code error;
    const auto size = std::filesystem::file_size(config_.log_file, error);
    if (error || static_cast<long long>(size) <= config_.max_log_bytes) {
        return;
    }
    std::error_code rename_error;
    // os.replace(): overwrites a stale .1 exactly like Python.
    std::filesystem::rename(config_.log_file, config_.log_file + ".1", rename_error);
    (void)rename_error;
}

HttpResponse CrashReceiver::handle(const HttpRequest& request) {
    if (request.method == "GET") {
        return bare_response(200);  // lightweight health check, no body
    }
    if (request.method != "POST") {
        HttpResponse response;
        response.status = 501;
        response.content_type = "text/plain; charset=utf-8";
        response.has_body = true;
        response.body = "Unsupported method ('" + request.method + "')";
        return response;
    }

    if (request.content_length_invalid) {
        return bare_response(400);
    }
    const long long length = request.content_length.value_or(0);
    if (length <= 0 || length > config_.max_body) {
        return bare_response(413);
    }
    if (!rate_ok()) {
        return bare_response(429);
    }

    const std::string body = request.read_body(static_cast<std::size_t>(length));
    if (!valid_utf8(body)) {
        return bare_response(400);
    }
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        return bare_response(400);
    }
    const json::Value& data = *parsed.value;

    // Sanitize: allowed fields only, strings capped in code points, bool/number verbatim.
    std::string record = "{";
    bool first = true;
    for (const char* field : kAllowedFields) {
        const json::Value* value = last_member(data, field);
        if (value == nullptr) {
            continue;
        }
        std::string encoded;
        if (value->kind == json::Kind::String) {
            encoded = py_string(take_code_points(value->text, config_.max_string));
        } else if (value->kind == json::Kind::Literal &&
                   (value->text == "true" || value->text == "false")) {
            encoded = value->text;
        } else if (value->kind == json::Kind::Literal && is_json_number(value->text)) {
            encoded = value->text;
        } else {
            continue;  // null / anything else: python drops it
        }
        if (!first) {
            record += ", ";
        }
        first = false;
        record += py_string(field);
        record += ": ";
        record += encoded;
    }
    if (!first) {
        record += ", ";
    }
    const auto received_at = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    record += "\"received_at\": ";
    record += std::to_string(received_at);
    record += "}";

    std::error_code ignored;
    rotate_if_needed();
    std::ofstream log(config_.log_file, std::ios::app | std::ios::binary);
    if (!log) {
        return bare_response(500);
    }
    log << record << '\n';
    log.flush();
    if (!log.good()) {
        return bare_response(500);
    }

    // message_for looks at the ORIGINAL package name (before sanitizing caps).
    const json::Value* package = last_member(data, "packageName");
    const std::string package_name =
        (package != nullptr && package->kind == json::Kind::String) ? package->text : std::string();
    if (const std::optional<SubmissionMessage> message = message_for(package_name);
        message.has_value()) {
        return message_response(*message);
    }
    return bare_response(204);
}

}  // namespace agora::server
