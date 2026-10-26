#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace agora::core::wire {

/**
 * The subset of okio's BufferedSource that the shell transport uses.
 *
 * The app adapter delegates to the real okio source; the host tests use a scripted byte source.
 * `request(n)` reports whether at least `n` bytes remain, `read_utf8` consumes and decodes the
 * remaining bytes, and `read_utf8_line_strict` follows okio's line contract: it returns the line
 * without the `\n` or `\r\n` terminator, throws Eof when no complete line fits within `limit`,
 * and leaves the unread bytes counted by `buffer_size()`.
 */
class Source {
public:
    virtual ~Source() = default;
    virtual bool request(std::int64_t byte_count) = 0;
    virtual std::string read_utf8() = 0;
    virtual bool exhausted() = 0;
    virtual std::string read_utf8_line_strict(std::int64_t limit) = 0;
    virtual std::int64_t buffer_size() = 0;
};

/** okio's EOFException, translated by the line wrapper. */
struct Eof {};

/**
 * BoundedWireRead.kt `readBoundedWireText`: require a positive finite limit, reject a source with
 * more than `limit` bytes, then decode everything that remains.
 */
std::string read_bounded_wire_text(Source& source, std::int64_t limit);

/**
 * BoundedWireRead.kt `readBoundedWireLine`: null at a clean EOF, the next line otherwise. An Eof
 * with more than `limit` bytes buffered means the event exceeded the transport limit; any other
 * Eof means the encrypted event line was incomplete.
 */
std::optional<std::string> read_bounded_wire_line(Source& source, std::int64_t limit);

}  // namespace agora::core::wire
