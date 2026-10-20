#pragma once

#include <string>
#include <string_view>

namespace agora::core {

/**
 * java.util.UUID.nameUUIDFromBytes: the MD5 name-based UUID (version 3) in canonical lowercase
 * form with hyphens.
 *
 * The app derives deterministic legacy Run ids and custom-provider ids this way; the algorithm is
 * fixed by the JDK, so both ports share one implementation.
 */
std::string uuid_v3(std::string_view name);

}  // namespace agora::core
