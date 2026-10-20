// Port of data/local/DatabaseCompatibility.kt.
//
// The Kotlin source catches `Exception`, not `Throwable`; Errors are never classified as
// Unreadable. The port models only the Exception path (the scenarios pin every JDK exception
// family the database gate can realistically see).
#include "agora/core/database_compatibility.hpp"

namespace agora::core::database_compatibility {

Classification classify(bool database_exists, int current_version,
                        std::optional<int> stored_version,
                        const std::optional<std::string>& read_error) {
    Classification result;
    if (!database_exists) {
        result.kind = Kind::Missing;
        return result;
    }
    if (read_error.has_value()) {
        result.kind = Kind::Unreadable;
        result.error = *read_error;
        return result;
    }

    const int stored = stored_version.value_or(0);
    result.stored_version = stored;
    if (stored > current_version) {
        result.kind = Kind::FutureVersion;
        result.supported_version = current_version;
    } else {
        result.kind = Kind::Supported;
    }
    return result;
}

bool can_open(const Classification& classification) {
    return classification.kind == Kind::Missing || classification.kind == Kind::Supported;
}

std::string describe(const Classification& classification) {
    switch (classification.kind) {
        case Kind::Missing:
            return "Missing";
        case Kind::Supported:
            return "Supported(storedVersion=" + std::to_string(classification.stored_version) + ")";
        case Kind::FutureVersion:
            return "FutureVersion(storedVersion=" + std::to_string(classification.stored_version) +
                   ", supportedVersion=" + std::to_string(classification.supported_version) + ")";
        case Kind::Unreadable:
            return "Unreadable(error=" + classification.error + ")";
    }
    return "Missing";
}

}  // namespace agora::core::database_compatibility
