#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agora::core::sandbox {

/**
 * normalizeVirtualPath: Kotlin whitespace trim, backslashes to '/', force a leading '/',
 * collapse repeated '/', and drop trailing '/' unless the result is the root "/".
 *
 * This is the virtual-path half of SandboxPathResolver.resolveSandboxPath; canonicalization,
 * mount-target creation and the traversal containment check remain platform concerns.
 */
std::string normalize_virtual_path(std::string_view path);

/**
 * globMatch: trim/normalize the pattern, anchor it to the sandbox root ("star-star-slash",
 * "/pattern", "/dir/pattern" or "star-star-slash/pattern"), then filter `files`
 * through PortableGlobMatcher in order.
 */
std::vector<std::string> glob_match(const std::vector<std::string>& files,
                                    std::string_view pattern);

}  // namespace agora::core::sandbox
