#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace agora::server {

/** The site file the Python tool patched (kept as the default argument). */
inline constexpr const char* kDefaultNginxSitePath = "/etc/nginx/sites-available/newoether.com";

/**
 * Inserts the /crash location block immediately before every `    location / {` anchor,
 * exactly like server/crash/patch_nginx.py: returns the unchanged text when the block is
 * already present, and nullopt when the anchor is missing (the Python tool exited with 2).
 */
std::optional<std::string> patch_nginx_text(std::string_view text);

/** Number of `location = /crash` occurrences, the counter the Python tool printed. */
std::size_t count_crash_locations(std::string_view text);

}  // namespace agora::server
