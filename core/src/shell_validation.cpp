#include "agora/core/shell_validation.hpp"

namespace agora::core::shell_tools {

std::optional<std::string> shell_command_validation_error(std::string_view command,
                                                          std::string_view workdir) {
    // Kotlin counts UTF-8 bytes, not chars; std::string_view::size() is the byte count of the
    // decoded UTF-8 text, so the comparison is identical to toByteArray(Charsets.UTF_8).size.
    if (command.size() > SHELL_COMMAND_MAX_BYTES) {
        return "command exceeds 64KB limit";
    }
    if (workdir.size() > SHELL_WORKDIR_MAX_BYTES) {
        return "workdir exceeds 32KB limit";
    }
    return std::nullopt;
}

}  // namespace agora::core::shell_tools
