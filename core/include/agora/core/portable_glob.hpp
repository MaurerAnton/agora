#pragma once

#include <string_view>

namespace agora::core::glob {

/**
 * PortableGlobMatcher.matches: a path-glob matcher that works on every supported API level.
 *
 * Backslashes normalize to '/', `*` stays inside one segment (`[^/]` repeated), `**` crosses
 * directories (any run of characters, optionally followed by a separator), `?` matches one
 * non-separator code point,
 * character classes are supported (`[!...]` negates) and an empty class is an invalid pattern
 * (the Kotlin code turns the regex `PatternSyntaxException` into `false`).
 */
bool matches(std::string_view pattern, std::string_view path);

}  // namespace agora::core::glob