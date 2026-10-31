#include "agora/server/patch_nginx.hpp"

namespace agora::server {
namespace {

constexpr const char* kAnchor = "    location / {";
constexpr const char* kMarker = "location = /crash";

// Byte-for-byte the block patch_nginx.py inserted (4-space indent, blank line before anchor).
constexpr const char* kBlock =
    "    location = /crash {\n"
    "        limit_except POST { deny all; }\n"
    "        client_max_body_size 64k;\n"
    "        proxy_pass http://127.0.0.1:8092/;\n"
    "        proxy_set_header Host $host;\n"
    "        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
    "        proxy_set_header X-Forwarded-Proto $scheme;\n"
    "    }\n"
    "\n";

}  // namespace

std::optional<std::string> patch_nginx_text(std::string_view text) {
    if (text.find(kMarker) != std::string_view::npos) {
        return std::string(text);  // already present; no change
    }
    if (text.find(kAnchor) == std::string_view::npos) {
        return std::nullopt;  // anchor not found
    }
    std::string patched;
    patched.reserve(text.size() + sizeof(kBlock) * 4);
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t anchor = text.find(kAnchor, cursor);
        if (anchor == std::string_view::npos) {
            patched.append(text.substr(cursor));
            break;
        }
        patched.append(text.substr(cursor, anchor - cursor));
        patched.append(kBlock);
        patched.append(text.substr(anchor, std::string_view(kAnchor).size()));
        cursor = anchor + std::string_view(kAnchor).size();
    }
    return patched;
}

std::size_t count_crash_locations(std::string_view text) {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = text.find(kMarker, cursor)) != std::string_view::npos) {
        ++count;
        cursor += 1;
    }
    return count;
}

}  // namespace agora::server
