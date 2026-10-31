#pragma once

#include <cstddef>
#include <string>

#include "agora/server/http_server.hpp"

namespace agora::server {

/** Configuration of the rating receiver; defaults match the Python service constants. */
struct RatingConfig {
    std::string db_path = "/var/lib/agora-rating/ratings.db";
    std::string host = "127.0.0.1";
    int port = 8091;
    long long max_content_length = 64 * 1024;  // mirrors nginx client_max_body_size
    std::size_t max_comment_length = 10000;
};

/** AGORA_RATING_DB / AGORA_RATING_HOST / AGORA_RATING_PORT, like the Python receiver. */
RatingConfig rating_config_from_env();

/**
 * Pure request handler with the Python receiver's exact status/body semantics:
 * GET -> 405 json, OPTIONS -> 204 (path-checked first), POST /api/rating -> validation chain
 * (invalid content length / empty request / too large / invalid json / json object required /
 * rating range) then a SQLite insert (schema failure -> 500 storage failure) and
 * `{"ok": true[, "message": ...]}` with the optional submission message.
 */
HttpResponse handle_rating_request(const HttpRequest& request, const RatingConfig& config);

}  // namespace agora::server
