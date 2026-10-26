#include "agora/core/bounded_wire_read.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace agora::core::wire {

std::string read_bounded_wire_text(Source& source, std::int64_t limit) {
    if (!(limit > 0 && limit < std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("Failed requirement.");
    }
    if (source.request(limit + 1)) {
        throw std::runtime_error("Response exceeds the " + std::to_string(limit) +
                                 "-byte transport limit");
    }
    return source.read_utf8();
}

std::optional<std::string> read_bounded_wire_line(Source& source, std::int64_t limit) {
    if (!(limit > 0 && limit < std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("Failed requirement.");
    }
    if (source.exhausted()) return std::nullopt;
    try {
        return source.read_utf8_line_strict(limit);
    } catch (const Eof&) {
        if (source.buffer_size() > limit) {
            throw std::runtime_error("Event exceeds the " + std::to_string(limit) +
                                     "-byte transport limit");
        }
        throw std::runtime_error("Incomplete encrypted event line");
    }
}

}  // namespace agora::core::wire
