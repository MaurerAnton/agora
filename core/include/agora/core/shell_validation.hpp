#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace agora::core::shell_tools {

// ---------------------------------------------------------------------------------------------
// ShellToolProvider.kt / ShellClient.kt - pre-flight size validation of a shell request
// ---------------------------------------------------------------------------------------------

// Limits from ShellClient.kt, checked as UTF-8 byte counts (String.toByteArray(UTF_8).size).
inline constexpr std::size_t SHELL_COMMAND_MAX_BYTES = 64 * 1024;
inline constexpr std::size_t SHELL_WORKDIR_MAX_BYTES = 32 * 1024;

// shellCommandValidationError from ShellToolProvider.kt: the command is checked first, then the
// workdir. Returns the exact error message the tool reports, or nullopt when the request passes.
std::optional<std::string> shell_command_validation_error(std::string_view command,
                                                          std::string_view workdir);

}  // namespace agora::core::shell_tools
