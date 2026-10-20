#pragma once

#include <optional>
#include <string>

namespace agora::core::database_compatibility {

// Port of data/local/DatabaseCompatibility.kt: the fail-closed inspection that runs before Room
// is allowed to open the durable database. Missing and supported databases may open; future
// versions and unreadable files stay closed until the user explicitly clears them.
//
// The Kotlin reader is `() -> Int` and may throw. The port keeps the same boundary: the caller
// passes the read result or the platform description of the exception it caught.

enum class Kind {
    Missing,
    Supported,
    FutureVersion,
    Unreadable,
};

struct Classification {
    Kind kind = Kind::Missing;
    int stored_version = 0;
    int supported_version = 0;
    std::string error;  // Unreadable only: the platform exception description
};

/**
 * classify: a missing database short-circuits before the reader (so a throwing reader still
 * yields Missing); a read failure becomes Unreadable; otherwise a strictly newer stored version
 * is a FutureVersion and everything else is Supported.
 */
Classification classify(bool database_exists, int current_version,
                        std::optional<int> stored_version,
                        const std::optional<std::string>& read_error);

/** canOpen: true exactly for Missing and Supported. */
bool can_open(const Classification& classification);

/** Kotlin data-class / data-object toString() of the sealed interface. */
std::string describe(const Classification& classification);

}  // namespace agora::core::database_compatibility
