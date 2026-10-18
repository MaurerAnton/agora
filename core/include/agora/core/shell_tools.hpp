#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::shell_tools {

// ---------------------------------------------------------------------------------------------
// ShellToolJson.kt
// ---------------------------------------------------------------------------------------------

/** parseToolArgs: the JSON object as ordered key → element pairs (last duplicate value wins at
 *  the first position); a blank/malformed/non-object payload yields the empty map. */
std::vector<std::pair<std::string, json::Value>> parse_tool_args(std::string_view arguments);

/** arg: JsonPrimitive.content or "" — the literal `null` primitive carries the text "null". */
std::string arg(const std::vector<std::pair<std::string, json::Value>>& args,
                std::string_view key);

/** boolArg: content.toBooleanStrictOrNull() == true (only "true"/"false" spellings). */
bool bool_arg(const std::vector<std::pair<std::string, json::Value>>& args,
              std::string_view key);

/** jsonError: {type?, error:"error", message, server?, command?} in that exact key order. */
std::string json_error(std::string_view type, std::string_view message,
                       const std::optional<std::string>& server,
                       const std::optional<std::string>& command);

// ---------------------------------------------------------------------------------------------
// BoundedFileReadJson.kt (+ ShellFileReadResult / shellFileLineCount from ShellClient.kt)
// ---------------------------------------------------------------------------------------------

struct ShellFileReadResult {
    std::string content;
    std::int32_t lines = 0;
    std::int32_t total_lines = 0;
    std::int64_t total_bytes = 0;
    std::int64_t returned_bytes = 0;
    std::int64_t offset = 0;
    std::int64_t limit = 0;
    bool truncated = false;
    std::optional<std::string> error;
};

/** shellFileLineCount: 0 for empty content, else newline count + 1. */
std::int32_t shell_file_line_count(std::string_view content);

/** boundedFileReadJson: the file_read JSON, truncated by a binary search over UTF-16 units so a
 *  returned slice never splits a surrogate pair; throws IllegalArgumentException (as
 *  ProviderRequestError) when max_length <= 0. */
std::string bounded_file_read_json(const std::string& server, const std::string& path,
                                   const ShellFileReadResult& result,
                                   std::int32_t max_length = 100000);

}  // namespace agora::core::shell_tools
