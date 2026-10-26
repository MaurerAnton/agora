#include "agora/core/sandbox_virtual_paths.hpp"

#include "agora/core/json.hpp"
#include "agora/core/portable_glob.hpp"

namespace agora::core::sandbox {
namespace {

std::string slash_normalized(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

}  // namespace

std::string normalize_virtual_path(std::string_view path) {
    const std::string raw = slash_normalized(json::trim(path));
    std::string absolute;
    if (json::is_blank(raw)) {
        absolute = "/";
    } else if (!raw.empty() && raw.front() == '/') {
        absolute = raw;
    } else {
        absolute = "/" + raw;
    }

    std::string collapsed;
    collapsed.reserve(absolute.size());
    for (const char c : absolute) {
        if (c == '/' && !collapsed.empty() && collapsed.back() == '/') continue;
        collapsed.push_back(c);
    }
    if (collapsed.size() > 1) {
        while (!collapsed.empty() && collapsed.back() == '/') collapsed.pop_back();
    }
    return collapsed;
}

std::vector<std::string> glob_match(const std::vector<std::string>& files,
                                    std::string_view pattern) {
    const std::string clean_pattern = slash_normalized(json::trim(pattern));
    std::string adjusted;
    if (json::is_blank(clean_pattern)) {
        adjusted = "/**";
    } else if (clean_pattern.front() == '/') {
        adjusted = clean_pattern;
    } else if (clean_pattern.find('/') != std::string::npos) {
        adjusted = "/" + clean_pattern;
    } else {
        adjusted = "/**/" + clean_pattern;
    }

    std::vector<std::string> matches;
    matches.reserve(files.size());
    for (const std::string& file : files) {
        if (glob::matches(adjusted, file)) matches.push_back(file);
    }
    return matches;
}

}  // namespace agora::core::sandbox
