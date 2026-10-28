#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace agora::core::font {

/** Result of reading TTF/OTF name-table metadata. */
struct FontMetadata {
    std::string family;
    std::optional<std::string> subfamily;
};

/**
 * Reads font family + subfamily from a TTF/OTF file via the name table.
 * Port of util/FontUtils.kt - pure stdlib, no platform dependencies.
 */
FontMetadata read_font_meta(const std::filesystem::path& file);

/** Reads just the font family name from a TTF/OTF file. */
std::string read_font_name(const std::filesystem::path& file);

}  // namespace agora::core::font