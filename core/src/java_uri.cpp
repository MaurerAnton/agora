#include "agora/core/java_uri.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/unicode.hpp"
#include "utf16.hpp"

// This translation follows java.net.URI (JDK 26) line by line: the parser, the string
// construction, the escaping/encoding helpers and the path normalizer are direct ports, with
// the exception mechanism reduced to a single SyntaxError marker type. The comments that
// matter for behaviour (JDK deviations, scanner conventions) are carried over verbatim.

namespace agora::core::java_uri {
namespace {

using U16 = std::u16string;

/** Stands for java.net.URISyntaxException; reason and position are not part of the contract. */
struct SyntaxError {};

[[noreturn]] void fail(const char* /*reason*/) { throw SyntaxError{}; }

// -- Character class masks, copied from java.net.URI ----------------------------------------

constexpr std::uint64_t L_DIGIT = 0x3FF000000000000ULL;
constexpr std::uint64_t H_DIGIT = 0ULL;
constexpr std::uint64_t L_UPALPHA = 0ULL;
constexpr std::uint64_t H_UPALPHA = 0x7FFFFFEULL;
constexpr std::uint64_t L_LOWALPHA = 0ULL;
constexpr std::uint64_t H_LOWALPHA = 0x7FFFFFE00000000ULL;
constexpr std::uint64_t L_ALPHA = L_LOWALPHA | L_UPALPHA;
constexpr std::uint64_t H_ALPHA = H_LOWALPHA | H_UPALPHA;
constexpr std::uint64_t L_ALPHANUM = L_DIGIT | L_ALPHA;
constexpr std::uint64_t H_ALPHANUM = H_DIGIT | H_ALPHA;
constexpr std::uint64_t L_HEX = L_DIGIT;
constexpr std::uint64_t H_HEX = 0x7E0000007EULL;
constexpr std::uint64_t L_MARK = 0x678200000000ULL;
constexpr std::uint64_t H_MARK = 0x4000000080000000ULL;
constexpr std::uint64_t L_UNRESERVED = L_ALPHANUM | L_MARK;
constexpr std::uint64_t H_UNRESERVED = H_ALPHANUM | H_MARK;
constexpr std::uint64_t L_RESERVED = 0xAC00985000000000ULL;
constexpr std::uint64_t H_RESERVED = 0x28000001ULL;
// The zero'th bit is used to indicate that escape pairs and non-US-ASCII characters are
// allowed; this is handled by the scanEscape method below.
constexpr std::uint64_t L_ESCAPED = 1ULL;
constexpr std::uint64_t H_ESCAPED = 0ULL;
constexpr std::uint64_t L_URIC = L_RESERVED | L_UNRESERVED | L_ESCAPED;
constexpr std::uint64_t H_URIC = H_RESERVED | H_UNRESERVED | H_ESCAPED;
constexpr std::uint64_t L_PCHAR = L_UNRESERVED | L_ESCAPED | 0x2400185000000000ULL;
constexpr std::uint64_t H_PCHAR = H_UNRESERVED | H_ESCAPED | 0x1ULL;
constexpr std::uint64_t L_PATH = L_PCHAR | 0x800800000000000ULL;
constexpr std::uint64_t H_PATH = H_PCHAR;
constexpr std::uint64_t L_DASH = 0x200000000000ULL;
constexpr std::uint64_t H_DASH = 0x0ULL;
constexpr std::uint64_t L_DOT = 0x400000000000ULL;
constexpr std::uint64_t H_DOT = 0x0ULL;
constexpr std::uint64_t L_USERINFO = L_UNRESERVED | L_ESCAPED | 0x2C00185000000000ULL;
constexpr std::uint64_t H_USERINFO = H_UNRESERVED | H_ESCAPED;
constexpr std::uint64_t L_REG_NAME = L_UNRESERVED | L_ESCAPED | 0x2C00185000000000ULL;
constexpr std::uint64_t H_REG_NAME = H_UNRESERVED | H_ESCAPED | 0x1ULL;
constexpr std::uint64_t L_SERVER = L_USERINFO | L_ALPHANUM | L_DASH | 0x400400000000000ULL;
constexpr std::uint64_t H_SERVER = H_USERINFO | H_ALPHANUM | H_DASH | 0x28000001ULL;
constexpr std::uint64_t L_SERVER_PERCENT = L_SERVER | 0x2000000000ULL;
constexpr std::uint64_t H_SERVER_PERCENT = H_SERVER;
constexpr std::uint64_t L_SCHEME = L_ALPHA | L_DIGIT | 0x680000000000ULL;
constexpr std::uint64_t H_SCHEME = H_ALPHA | H_DIGIT;
constexpr std::uint64_t L_SCOPE_ID = L_ALPHANUM | 0x400000000000ULL;
constexpr std::uint64_t H_SCOPE_ID = H_ALPHANUM | 0x80000000ULL;

// Tell whether the given character is permitted by the given mask pair
bool match(char16_t c, std::uint64_t low_mask, std::uint64_t high_mask) {
    if (c == 0)  // 0 doesn't have a slot in the mask. So, it never matches.
        return false;
    if (c < 64) return ((1ULL << c) & low_mask) != 0;
    if (c < 128) return ((1ULL << (c - 64)) & high_mask) != 0;
    return false;
}

/** Character.isSpaceChar on a UTF-16 unit (only BMP characters are space chars). */
bool is_space_char(char16_t c) { return (unicode::flags_of(c) & unicode::kJvmSpace) != 0; }

/** Character.isISOControl on a UTF-16 unit. */
bool is_iso_control(char16_t c) { return c <= 0x1F || (c >= 0x7F && c <= 0x9F); }

// -- The parse state, the union of the java.net.URI Parser fields and the URI fields --------

struct Impl {
    U16 input;
    std::optional<U16> scheme;
    std::optional<U16> fragment;
    std::optional<U16> authority;
    std::optional<U16> user_info;
    std::optional<U16> host;
    int port = -1;
    std::optional<U16> path;
    std::optional<U16> query;
    std::optional<U16> scheme_specific_part;
    bool require_server_authority = false;
    int ipv6_byte_count = 0;
};

// -- Scanner primitives (java.net.URI.Parser) -----------------------------------------------

bool at_char(const Impl& impl, int start, int end, char16_t c) {
    return (start < end) && (impl.input[static_cast<std::size_t>(start)] == c);
}

bool at_str(const Impl& impl, int start, int end, std::u16string_view s) {
    int p = start;
    const std::size_t sn = s.size();
    if (end - p < static_cast<int>(sn)) return false;
    std::size_t i = 0;
    while (i < sn) {
        if (impl.input[static_cast<std::size_t>(p++)] != s[i]) break;
        i++;
    }
    return i == sn;
}

// Scan a specific char: if the char at the given start position is equal to c, return the
// index of the next char; otherwise, return the start position.
int scan_char(const Impl& impl, int start, int end, char16_t c) {
    if ((start < end) && (impl.input[static_cast<std::size_t>(start)] == c)) return start + 1;
    return start;
}

// Scan forward; stop at the first char in err (-1), the first char in stop (index of the
// preceding char), or the end (length).
int scan_err_stop(const Impl& impl, int start, int end, std::u16string_view err,
                  std::u16string_view stop) {
    int p = start;
    while (p < end) {
        const char16_t c = impl.input[static_cast<std::size_t>(p)];
        if (err.find(c) != std::u16string_view::npos) return -1;
        if (stop.find(c) != std::u16string_view::npos) break;
        p++;
    }
    return p;
}

int scan_stop(const Impl& impl, int start, int end, std::u16string_view stop) {
    int p = start;
    while (p < end) {
        const char16_t c = impl.input[static_cast<std::size_t>(p)];
        if (stop.find(c) != std::u16string_view::npos) break;
        p++;
    }
    return p;
}

// Scan a potential escape sequence, starting at the given position, with the given first char.
int scan_escape(const Impl& impl, int start, int n, char16_t first) {
    const int p = start;
    const char16_t c = first;
    if (c == u'%') {
        // Process escape pair
        if ((p + 3 <= n) && match(impl.input[static_cast<std::size_t>(p + 1)], L_HEX, H_HEX) &&
            match(impl.input[static_cast<std::size_t>(p + 2)], L_HEX, H_HEX)) {
            return p + 3;
        }
        fail("Malformed escape pair");
    } else if ((c > 128) && !is_space_char(c) && !is_iso_control(c)) {
        // Allow unescaped but visible non-US-ASCII chars
        return p + 1;
    }
    return p;
}

// Scan chars that match the given mask pair
int scan_mask(const Impl& impl, int start, int n, std::uint64_t low_mask,
              std::uint64_t high_mask) {
    int p = start;
    while (p < n) {
        const char16_t c = impl.input[static_cast<std::size_t>(p)];
        if (match(c, low_mask, high_mask)) {
            p++;
            continue;
        }
        if ((low_mask & L_ESCAPED) != 0) {
            const int q = scan_escape(impl, p, n, c);
            if (q > p) {
                p = q;
                continue;
            }
        }
        break;
    }
    return p;
}

void check_chars(const Impl& impl, int start, int end, std::uint64_t low_mask,
                 std::uint64_t high_mask, const char* /*what*/) {
    const int p = scan_mask(impl, start, end, low_mask, high_mask);
    if (p < end) fail("Illegal character");
}

void check_char(const Impl& impl, int p, std::uint64_t low_mask, std::uint64_t high_mask,
                const char* what) {
    check_chars(impl, p, p + 1, low_mask, high_mask, what);
}

// Integer.parseInt(s, from, to, 10) for digit-only ranges: overflow rejects the literal the
// same way NumberFormatException does.
bool parse_int_digits(const U16& s, int from, int to, std::int64_t& out) {
    std::int64_t value = 0;
    for (int p = from; p < to; ++p) {
        const char16_t c = s[static_cast<std::size_t>(p)];
        if (c < u'0' || c > u'9') return false;
        value = value * 10 + (c - u'0');
        if (value > 0x7FFFFFFFLL) return false;  // Integer.MAX_VALUE
    }
    out = value;
    return true;
}

// -- Parsing (java.net.URI.Parser) -----------------------------------------------------------

int parse_hierarchical(Impl& impl, int start, int n);
int parse_authority(Impl& impl, int start, int n);
int parse_server(Impl& impl, int start, int n, bool skip_parse_exception);
int parse_ipv4_address(Impl& impl, int start, int n);
int parse_hostname(Impl& impl, int start, int n, bool skip_parse_exception);
int parse_ipv6_reference(Impl& impl, int start, int n);
int scan_ipv4_address(Impl& impl, int start, int n, bool strict);
int scan_byte(Impl& impl, int start, int n);
int scan_hex_post(Impl& impl, int start, int n);
int scan_hex_seq(Impl& impl, int start, int n);

// [<scheme>:]<scheme-specific-part>[#<fragment>]
void parse_uri(Impl& impl, bool rsa) {
    impl.require_server_authority = rsa;
    const int n = static_cast<int>(impl.input.size());
    int p = scan_err_stop(impl, 0, n, u"/?#", u":");
    if ((p >= 0) && at_char(impl, p, n, u':')) {
        if (p == 0) fail("Expected scheme name");
        check_char(impl, 0, L_ALPHA, H_ALPHA, "scheme name");
        check_chars(impl, 1, p, L_SCHEME, H_SCHEME, "scheme name");
        impl.scheme = impl.input.substr(0, static_cast<std::size_t>(p));
        p++;  // Skip ':'
        if (at_char(impl, p, n, u'/')) {
            p = parse_hierarchical(impl, p, n);
        } else {
            // opaque; need to create the schemeSpecificPart
            const int q = scan_stop(impl, p, n, u"#");
            if (q <= p) fail("Expected scheme-specific part");
            check_chars(impl, p, q, L_URIC, H_URIC, "opaque part");
            impl.scheme_specific_part = impl.input.substr(static_cast<std::size_t>(p),
                                                          static_cast<std::size_t>(q - p));
            p = q;
        }
    } else {
        p = parse_hierarchical(impl, 0, n);
    }
    if (at_char(impl, p, n, u'#')) {
        check_chars(impl, p + 1, n, L_URIC, H_URIC, "fragment");
        impl.fragment = impl.input.substr(static_cast<std::size_t>(p + 1),
                                          static_cast<std::size_t>(n - p - 1));
        p = n;
    }
    if (p < n) fail("end of URI");
}

// [//authority]<path>[?<query>]
//
// DEVIATION from RFC2396: We allow an empty authority component as long as it's followed by a
// non-empty path, query component, or fragment component. This is so that URIs such as
// "file:///foo/bar" will parse. This seems to be the intent of RFC2396, though the grammar
// does not permit it. If the authority is empty then the userInfo, host, and port components
// are undefined.
//
// DEVIATION from RFC2396: We allow empty relative paths. This seems to be the intent of
// RFC2396, but the grammar does not permit it. The primary consequence of this deviation is
// that "#f" parses as a relative URI with an empty path.
int parse_hierarchical(Impl& impl, int start, int n) {
    int p = start;
    if (at_char(impl, p, n, u'/') && at_char(impl, p + 1, n, u'/')) {
        p += 2;
        const int q = scan_stop(impl, p, n, u"/?#");
        if (q > p) {
            p = parse_authority(impl, p, q);
        } else if (q < n) {
            // DEVIATION: Allow empty authority prior to non-empty path, query component or
            // fragment identifier
        } else {
            fail("Expected authority");
        }
    }
    int q = scan_stop(impl, p, n, u"?#");  // DEVIATION: May be empty
    check_chars(impl, p, q, L_PATH, H_PATH, "path");
    impl.path = impl.input.substr(static_cast<std::size_t>(p),
                                  static_cast<std::size_t>(q - p));
    p = q;
    if (at_char(impl, p, n, u'?')) {
        p++;
        q = scan_stop(impl, p, n, u"#");
        check_chars(impl, p, q, L_URIC, H_URIC, "query");
        impl.query = impl.input.substr(static_cast<std::size_t>(p),
                                       static_cast<std::size_t>(q - p));
        p = q;
    }
    return p;
}

// authority = server | reg_name
//
// Ambiguity: An authority that is a registry name rather than a server might have a prefix
// that parses as a server. We use the fact that the authority component is always followed by
// '/' or the end of the input string to resolve this: If the complete authority did not parse
// as a server then we try to parse it as a registry name.
int parse_authority(Impl& impl, int start, int n) {
    const int p = start;
    int q = p;
    int qreg = p;
    std::exception_ptr ex;
    bool server_chars;
    bool reg_chars;
    if (scan_stop(impl, p, n, u"]") > p) {
        // contains a literal IPv6 address, therefore % is allowed
        server_chars = (scan_mask(impl, p, n, L_SERVER_PERCENT, H_SERVER_PERCENT) == n);
    } else {
        server_chars = (scan_mask(impl, p, n, L_SERVER, H_SERVER) == n);
    }
    qreg = scan_mask(impl, p, n, L_REG_NAME, H_REG_NAME);
    reg_chars = (qreg == n);
    if (reg_chars && !server_chars) {
        // Must be a registry-based authority
        impl.authority = impl.input.substr(static_cast<std::size_t>(p),
                                           static_cast<std::size_t>(n - p));
        return n;
    }
    // When parsing a URI, skip creating exception objects if the server-based authority is not
    // required and the registry parse is successful.
    const bool skip_parse_exception = (!impl.require_server_authority && reg_chars);
    if (server_chars) {
        // Might be (probably is) a server-based authority, so attempt to parse it as such. If
        // the attempt fails, try to treat it as a registry-based authority.
        try {
            q = parse_server(impl, p, n, skip_parse_exception);
            if (q < n) {
                if (skip_parse_exception) {
                    impl.user_info.reset();
                    impl.host.reset();
                    impl.port = -1;
                    q = p;
                } else {
                    fail("Expected end of authority");
                }
            } else {
                impl.authority = impl.input.substr(static_cast<std::size_t>(p),
                                                   static_cast<std::size_t>(n - p));
            }
        } catch (const SyntaxError&) {
            // Undo results of failed parse
            impl.user_info.reset();
            impl.host.reset();
            impl.port = -1;
            if (impl.require_server_authority) {
                // If we're insisting upon a server-based authority, then just re-throw the
                // exception
                throw;
            }
            // Save the exception in case it doesn't parse as a registry either
            ex = std::current_exception();
            q = p;
        }
    }
    if (q < n) {
        if (reg_chars) {
            // Registry-based authority
            impl.authority = impl.input.substr(static_cast<std::size_t>(p),
                                               static_cast<std::size_t>(n - p));
        } else if (ex) {
            // Re-throw exception; it was probably due to a malformed IPv6 address
            std::rethrow_exception(ex);
        } else {
            fail("Illegal character in authority");
        }
    }
    return n;
}

// [<userinfo>@]<host>[:<port>]
int parse_server(Impl& impl, int start, int n, bool skip_parse_exception) {
    int p = start;
    int q;
    // userinfo
    q = scan_err_stop(impl, p, n, u"/?#", u"@");
    if ((q >= p) && at_char(impl, q, n, u'@')) {
        check_chars(impl, p, q, L_USERINFO, H_USERINFO, "user info");
        impl.user_info = impl.input.substr(static_cast<std::size_t>(p),
                                           static_cast<std::size_t>(q - p));
        p = q + 1;  // Skip '@'
    }
    // hostname, IPv4 address, or IPv6 address
    if (at_char(impl, p, n, u'[')) {
        // DEVIATION from RFC2396: Support IPv6 addresses, per RFC2732
        p++;
        q = scan_err_stop(impl, p, n, u"/?#", u"]");
        if ((q > p) && at_char(impl, q, n, u']')) {
            // look for a "%" scope id
            const int r = scan_stop(impl, p, q, u"%");
            if (r > p) {
                parse_ipv6_reference(impl, p, r);
                if (r + 1 == q) {
                    fail("scope id expected");
                }
                check_chars(impl, r + 1, q, L_SCOPE_ID, H_SCOPE_ID, "scope id");
            } else {
                parse_ipv6_reference(impl, p, q);
            }
            impl.host = impl.input.substr(static_cast<std::size_t>(p - 1),
                                          static_cast<std::size_t>(q + 1 - (p - 1)));
            p = q + 1;
        } else {
            fail("Expected closing bracket for IPv6 address");
        }
    } else {
        q = parse_ipv4_address(impl, p, n);
        if (q <= p) q = parse_hostname(impl, p, n, skip_parse_exception);
        p = q;
    }
    // port
    if (at_char(impl, p, n, u':')) {
        p++;
        q = scan_stop(impl, p, n, u"/");
        if (q > p) {
            check_chars(impl, p, q, L_DIGIT, H_DIGIT, "port number");
            std::int64_t value = 0;
            if (!parse_int_digits(impl.input, p, q, value)) {
                fail("Malformed port number");
            }
            impl.port = static_cast<int>(value);
            p = q;
        }
    } else if (p < n && skip_parse_exception) {
        return p;
    }
    if (p < n) fail("Expected port number");
    return p;
}

// Scan a string of decimal digits whose value fits in a byte
int scan_byte(Impl& impl, int start, int n) {
    const int p = start;
    const int q = scan_mask(impl, p, n, L_DIGIT, H_DIGIT);
    if (q <= p) return q;
    // Handle leading zeros
    int i = p, j;
    while ((j = scan_char(impl, i, q, u'0')) > i) i = j;
    // Calculate the number of significant digits (after leading zeros)
    const int significant_digits_num = q - i;
    if (significant_digits_num < 3) return q;  // definitely < 255
    // If more than 3 significant digits, it's definitely > 255
    if (significant_digits_num > 3) return p;
    std::int64_t value = 0;
    parse_int_digits(impl.input, p, q, value);  // at most 3 significant digits: no overflow
    if (value > 255) return p;
    return q;
}

// Scan an IPv4 address.
//
// If the strict argument is true then we require that the given interval contain nothing
// besides an IPv4 address; if it is false then we only require that it start with an IPv4
// address.
//
// If the interval does not contain or start with (depending upon the strict argument) a legal
// IPv4 address characters then we return -1 immediately; otherwise we insist that these
// characters parse as a legal IPv4 address and throw an exception on failure.
//
// We assume that any string of decimal digits and dots must be an IPv4 address. It won't parse
// as a hostname anyway, so making that assumption here allows more meaningful exceptions to be
// thrown.
int scan_ipv4_address(Impl& impl, int start, int n, bool strict) {
    int p = start;
    int q;
    const int m = scan_mask(impl, p, n, L_DIGIT | L_DOT, H_DIGIT | H_DOT);
    if ((m <= p) || (strict && (m != n))) return -1;
    for (;;) {
        // Per RFC2732: At most three digits per byte
        // Further constraint: Each element fits in a byte
        if ((q = scan_byte(impl, p, m)) <= p) break;
        p = q;
        if ((q = scan_char(impl, p, m, u'.')) <= p) break;
        p = q;
        if ((q = scan_byte(impl, p, m)) <= p) break;
        p = q;
        if ((q = scan_char(impl, p, m, u'.')) <= p) break;
        p = q;
        if ((q = scan_byte(impl, p, m)) <= p) break;
        p = q;
        if ((q = scan_char(impl, p, m, u'.')) <= p) break;
        p = q;
        if ((q = scan_byte(impl, p, m)) <= p) break;
        p = q;
        if (q < m) break;
        return q;
    }
    if (strict) fail("Malformed IPv4 address");
    return -1;
}

// Take an IPv4 address: Throw an exception if the given interval contains anything except an
// IPv4 address
int take_ipv4_address(Impl& impl, int start, int n) {
    const int p = scan_ipv4_address(impl, start, n, true);
    if (p <= start) fail("Expected IPv4 address");
    return p;
}

// Attempt to parse an IPv4 address, returning -1 on failure but allowing the given interval to
// contain [:<characters>] after the IPv4 address.
int parse_ipv4_address(Impl& impl, int start, int n) {
    int p;
    try {
        p = scan_ipv4_address(impl, start, n, false);
    } catch (const SyntaxError&) {
        return -1;
    }
    if (p == -1) {
        return p;
    }
    if (p > start && p < n) {
        // IPv4 address is followed by something - check that it's a ":" as this is the only
        // valid character to follow an address.
        if (impl.input[static_cast<std::size_t>(p)] != u':') {
            return -1;
        }
    }
    if (p > start) {
        impl.host = impl.input.substr(static_cast<std::size_t>(start),
                                      static_cast<std::size_t>(p - start));
    }
    return p;
}

// hostname = domainlabel [ "." ] | 1*( domainlabel "." ) toplabel [ "." ]
// domainlabel = alphanum | alphanum *( alphanum | "-" ) alphanum
// toplabel = alpha | alpha *( alphanum | "-" ) alphanum
int parse_hostname(Impl& impl, int start, int n, bool skip_parse_exception) {
    int p = start;
    int q;
    int l = -1;  // Start of last parsed label
    do {
        // domainlabel = alphanum [ *( alphanum | "-" ) alphanum ]
        q = scan_mask(impl, p, n, L_ALPHANUM, H_ALPHANUM);
        if (q <= p) break;
        l = p;
        p = q;
        q = scan_mask(impl, p, n, L_ALPHANUM | L_DASH, H_ALPHANUM | H_DASH);
        if (q > p) {
            if (impl.input[static_cast<std::size_t>(q - 1)] == u'-') {
                fail("Illegal character in hostname");
            }
            p = q;
        }
        q = scan_char(impl, p, n, u'.');
        if (q <= p) break;
        p = q;
    } while (p < n);
    if ((p < n) && !at_char(impl, p, n, u':')) {
        if (skip_parse_exception) {
            return p;
        }
        fail("Illegal character in hostname");
    }
    if (l < 0) fail("Expected hostname");
    // for a fully qualified hostname check that the rightmost label starts with an alpha
    // character.
    if (l > start && !match(impl.input[static_cast<std::size_t>(l)], L_ALPHA, H_ALPHA)) {
        fail("Illegal character in hostname");
    }
    impl.host = impl.input.substr(static_cast<std::size_t>(start),
                                  static_cast<std::size_t>(p - start));
    return p;
}

// IPv6 address parsing, from RFC2373: IPv6 Addressing Architecture. We use the revised grammar
// from the JDK comment block (hexseq/hexpost) and the same byte-count constraints.
int parse_ipv6_reference(Impl& impl, int start, int n) {
    int p = start;
    int q;
    bool compressed_zeros = false;
    q = scan_hex_seq(impl, p, n);
    if (q > p) {
        p = q;
        if (at_str(impl, p, n, u"::")) {
            compressed_zeros = true;
            p = scan_hex_post(impl, p + 2, n);
        } else if (at_char(impl, p, n, u':')) {
            p = take_ipv4_address(impl, p + 1, n);
            impl.ipv6_byte_count += 4;
        }
    } else if (at_str(impl, p, n, u"::")) {
        compressed_zeros = true;
        p = scan_hex_post(impl, p + 2, n);
    }
    if (p < n) fail("Malformed IPv6 address");
    if (impl.ipv6_byte_count > 16) fail("IPv6 address too long");
    if (!compressed_zeros && impl.ipv6_byte_count < 16) fail("IPv6 address too short");
    if (compressed_zeros && impl.ipv6_byte_count == 16) fail("Malformed IPv6 address");
    return p;
}

int scan_hex_post(Impl& impl, int start, int n) {
    int p = start;
    int q;
    if (p == n) return p;
    q = scan_hex_seq(impl, p, n);
    if (q > p) {
        p = q;
        if (at_char(impl, p, n, u':')) {
            p++;
            p = take_ipv4_address(impl, p, n);
            impl.ipv6_byte_count += 4;
        }
    } else {
        p = take_ipv4_address(impl, p, n);
        impl.ipv6_byte_count += 4;
    }
    return p;
}

// Scan a hex sequence; return -1 if one could not be scanned
int scan_hex_seq(Impl& impl, int start, int n) {
    int p = start;
    int q;
    q = scan_mask(impl, p, n, L_HEX, H_HEX);
    if (q <= p) return -1;
    if (at_char(impl, q, n, u'.')) return -1;  // Beginning of IPv4 address
    if (q > p + 4) fail("IPv6 hexadecimal digit sequence too long");
    impl.ipv6_byte_count += 2;
    p = q;
    while (p < n) {
        if (!at_char(impl, p, n, u':')) break;
        if (at_char(impl, p + 1, n, u':')) break;  // "::"
        p++;
        q = scan_mask(impl, p, n, L_HEX, H_HEX);
        if (q <= p) fail("Expected digits for an IPv6 address");
        if (at_char(impl, q, n, u'.')) {  // Beginning of IPv4 address
            p--;
            break;
        }
        if (q > p + 4) fail("IPv6 hexadecimal digit sequence too long");
        impl.ipv6_byte_count += 2;
        p = q;
    }
    return p;
}

// -- Path normalization ----------------------------------------------------------------------

// The following algorithm for path normalization avoids the creation of a string object for
// each segment by using a single char array and editing it in place. The array is first split
// into segments, replacing each slash with '\0' and creating a segment-index array. We then
// walk through both arrays, removing ".", "..", and other segments as necessary by setting
// their entries in the index array to -1.

// Check the given path to see if it might need normalization. Return -1 if no further
// normalization is possible, otherwise return the number of segments found.
int needs_normalization(const U16& path) {
    bool normal = true;
    int ns = 0;                                        // Number of segments
    const int end = static_cast<int>(path.size()) - 1;  // Index of last char in path
    int p = 0;                                          // Index of next char in path
    // Skip initial slashes
    while (p <= end) {
        if (path[static_cast<std::size_t>(p)] != u'/') break;
        p++;
    }
    if (p > 1) normal = false;
    // Scan segments
    while (p <= end) {
        // Looking at "." or ".." ?
        if ((path[static_cast<std::size_t>(p)] == u'.') &&
            ((p == end) ||
             ((path[static_cast<std::size_t>(p + 1)] == u'/') ||
              ((path[static_cast<std::size_t>(p + 1)] == u'.') &&
               ((p + 1 == end) || (path[static_cast<std::size_t>(p + 2)] == u'/')))))) {
            normal = false;
        }
        ns++;
        // Find beginning of next segment
        while (p <= end) {
            if (path[static_cast<std::size_t>(p++)] != u'/') continue;
            // Skip redundant slashes
            while (p <= end) {
                if (path[static_cast<std::size_t>(p)] != u'/') break;
                normal = false;
                p++;
            }
            break;
        }
    }
    return normal ? -1 : ns;
}

// Split the given path into segments, replacing slashes with nulls and filling in the given
// segment-index array.
void split_path(U16& path, std::vector<int>& segs) {
    const int end = static_cast<int>(path.size()) - 1;  // Index of last char in path
    int p = 0;                                          // Index of next char in path
    int i = 0;                                          // Index of current segment
    // Skip initial slashes
    while (p <= end) {
        if (path[static_cast<std::size_t>(p)] != u'/') break;
        path[static_cast<std::size_t>(p)] = u'\0';
        p++;
    }
    while (p <= end) {
        // Note start of segment
        segs[static_cast<std::size_t>(i++)] = p++;
        // Find beginning of next segment
        while (p <= end) {
            if (path[static_cast<std::size_t>(p++)] != u'/') continue;
            path[static_cast<std::size_t>(p - 1)] = u'\0';
            // Skip redundant slashes
            while (p <= end) {
                if (path[static_cast<std::size_t>(p)] != u'/') break;
                path[static_cast<std::size_t>(p++)] = u'\0';
            }
            break;
        }
    }
}

// Join the segments in the given path according to the given segment-index array, ignoring
// those segments whose index entries have been set to -1, and inserting slashes as needed.
// Return the length of the resulting path.
int join_path(U16& path, const std::vector<int>& segs) {
    const int ns = static_cast<int>(segs.size());        // Number of segments
    const int end = static_cast<int>(path.size()) - 1;   // Index of last char in path
    int p = 0;                                           // Index of next path char to write
    if (path[0] == u'\0') {
        // Restore initial slash for absolute paths
        path[0] = u'/';
        p++;
    }
    for (int i = 0; i < ns; i++) {
        int q = segs[static_cast<std::size_t>(i)];  // Current segment
        if (q == -1)
            // Ignore this segment
            continue;
        if (p == q) {
            // We're already at this segment, so just skip to its end
            while ((p <= end) && (path[static_cast<std::size_t>(p)] != u'\0')) p++;
            if (p <= end) {
                // Preserve trailing slash
                path[static_cast<std::size_t>(p++)] = u'/';
            }
        } else if (p < q) {
            // Copy q down to p
            while ((q <= end) && (path[static_cast<std::size_t>(q)] != u'\0')) {
                path[static_cast<std::size_t>(p++)] = path[static_cast<std::size_t>(q++)];
            }
            if (q <= end) {
                // Preserve trailing slash
                path[static_cast<std::size_t>(p++)] = u'/';
            }
        }
    }
    return p;
}

// Remove "." segments from the given path, and remove segment pairs consisting of a non-".."
// segment followed by a ".." segment.
void remove_dots(U16& path, std::vector<int>& segs) {
    const int ns = static_cast<int>(segs.size());
    const int end = static_cast<int>(path.size()) - 1;
    for (int i = 0; i < ns; i++) {
        int dots = 0;  // Number of dots found (0, 1, or 2)
        // Find next occurrence of "." or ".."
        do {
            const int p = segs[static_cast<std::size_t>(i)];
            if (path[static_cast<std::size_t>(p)] == u'.') {
                if (p == end) {
                    dots = 1;
                    break;
                } else if (path[static_cast<std::size_t>(p + 1)] == u'\0') {
                    dots = 1;
                    break;
                } else if ((path[static_cast<std::size_t>(p + 1)] == u'.') &&
                           ((p + 1 == end) || (path[static_cast<std::size_t>(p + 2)] == u'\0'))) {
                    dots = 2;
                    break;
                }
            }
            i++;
        } while (i < ns);
        if (dots == 0) break;
        if (dots == 1) {
            // Remove this occurrence of "."
            segs[static_cast<std::size_t>(i)] = -1;
        } else {
            // If there is a preceding non-".." segment, remove both that segment and this
            // occurrence of ".."; otherwise, leave this ".." segment as-is.
            int j = i - 1;
            while (j >= 0 && segs[static_cast<std::size_t>(j)] == -1) j--;
            if (j >= 0) {
                const int q = segs[static_cast<std::size_t>(j)];
                if (!((path[static_cast<std::size_t>(q)] == u'.') &&
                      (path[static_cast<std::size_t>(q + 1)] == u'.') &&
                      (path[static_cast<std::size_t>(q + 2)] == u'\0'))) {
                    segs[static_cast<std::size_t>(i)] = -1;
                    segs[static_cast<std::size_t>(j)] = -1;
                }
            }
        }
    }
}

// DEVIATION: If the normalized path is relative, and if the first segment could be parsed as a
// scheme name, then prepend a "." segment
void maybe_add_leading_dot(U16& path, std::vector<int>& segs) {
    if (path[0] == u'\0')
        // The path is absolute
        return;
    const int ns = static_cast<int>(segs.size());
    int f = 0;  // Index of first segment
    while (f < ns) {
        if (segs[static_cast<std::size_t>(f)] >= 0) break;
        f++;
    }
    if ((f >= ns) || (f == 0))
        // The path is empty, or else the original first segment survived, in which case we
        // already know that no leading "." is needed
        return;
    int p = segs[static_cast<std::size_t>(f)];
    const int size = static_cast<int>(path.size());
    while ((p < size) && (path[static_cast<std::size_t>(p)] != u':') &&
           (path[static_cast<std::size_t>(p)] != u'\0')) {
        p++;
    }
    if (p >= size || path[static_cast<std::size_t>(p)] == u'\0')
        // No colon in first segment, so no "." needed
        return;
    // At this point we know that the first segment is unused, hence we can insert a "."
    // segment at that position
    path[0] = u'.';
    path[1] = u'\0';
    segs[0] = 0;
}

// Normalize the given path string, reporting whether the result differs from the input (the
// JDK returns the very same String object in that case, and normalize(URI) relies on it).
bool normalize_path(const U16& ps, U16& out) {
    // Does this path need normalization?
    const int ns = needs_normalization(ps);  // Number of segments
    if (ns < 0)
        // Nope -- just return it
        return false;
    U16 path = ps;  // Path in char-array form
    // Split path into segments
    std::vector<int> segs(static_cast<std::size_t>(ns), 0);  // Segment-index array
    split_path(path, segs);
    // Remove dots
    remove_dots(path, segs);
    // Prevent scheme-name confusion
    maybe_add_leading_dot(path, segs);
    // Join the remaining segments and return the result
    U16 s = path.substr(0, static_cast<std::size_t>(join_path(path, segs)));
    if (s == ps) {
        // string was already normalized
        return false;
    }
    out = std::move(s);
    return true;
}

// -- Escaping and encoding --------------------------------------------------------------------

constexpr char16_t kHexDigits[16] = {u'0', u'1', u'2', u'3', u'4', u'5', u'6', u'7',
                                     u'8', u'9', u'A', u'B', u'C', u'D', u'E', u'F'};

void append_escape(U16& sb, unsigned char b) {
    sb.push_back(u'%');
    sb.push_back(kHexDigits[(b >> 4) & 0x0F]);
    sb.push_back(kHexDigits[(b >> 0) & 0x0F]);
}

void append_encoded(U16& sb, char16_t c) {
    const std::u16string one(1, c);
    for (const char raw : utf16::to_utf8(one)) {
        const auto b = static_cast<unsigned char>(raw);
        if (b >= 0x80) {
            append_escape(sb, b);
        } else {
            sb.push_back(static_cast<char16_t>(b));
        }
    }
}

// Quote any characters in s that are not permitted by the given mask pair
U16 quote(const U16& s, std::uint64_t low_mask, std::uint64_t high_mask) {
    U16 sb;
    bool writing = false;  // sb == null
    const bool allow_non_ascii = ((low_mask & L_ESCAPED) != 0);
    for (std::size_t i = 0; i < s.size(); i++) {
        const char16_t c = s[i];
        if (c < u'\u0080') {
            if (!match(c, low_mask, high_mask)) {
                if (!writing) {
                    writing = true;
                    sb = s.substr(0, i);
                }
                append_escape(sb, static_cast<unsigned char>(c));
            } else {
                if (writing) sb.push_back(c);
            }
        } else if (allow_non_ascii && (is_space_char(c) || is_iso_control(c))) {
            if (!writing) {
                writing = true;
                sb = s.substr(0, i);
            }
            append_encoded(sb, c);
        } else {
            if (writing) sb.push_back(c);
        }
    }
    return writing ? std::move(sb) : s;
}

// Encodes all characters >= \u0080 into escaped, normalized UTF-8 octets, assuming that s is
// otherwise legal. The NFC normalization step of the JDK is out of scope (see header).
U16 encode(const U16& s) {
    const std::size_t n = s.size();
    if (n == 0) return s;
    // First check whether we actually need to encode
    for (std::size_t i = 0;;) {
        if (s[i] >= u'\u0080') break;
        if (++i >= n) return s;
    }
    U16 sb;
    std::size_t i = 0;
    while (i < n) {
        const char16_t c = s[i];
        if (c < u'\u0080') {
            sb.push_back(c);
            ++i;
            continue;
        }
        std::uint32_t code_point = 0;
        std::size_t used = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            code_point = 0x10000 + ((static_cast<std::uint32_t>(c) - 0xD800) << 10) +
                         (static_cast<std::uint32_t>(s[i + 1]) - 0xDC00);
            used = 2;
        } else {
            code_point = c;
        }
        unsigned char bytes[4];
        int count = 0;
        if (code_point < 0x800) {
            bytes[count++] = static_cast<unsigned char>(0xC0 | (code_point >> 6));
            bytes[count++] = static_cast<unsigned char>(0x80 | (code_point & 0x3F));
        } else if (code_point < 0x10000) {
            bytes[count++] = static_cast<unsigned char>(0xE0 | (code_point >> 12));
            bytes[count++] = static_cast<unsigned char>(0x80 | ((code_point >> 6) & 0x3F));
            bytes[count++] = static_cast<unsigned char>(0x80 | (code_point & 0x3F));
        } else {
            bytes[count++] = static_cast<unsigned char>(0xF0 | (code_point >> 18));
            bytes[count++] = static_cast<unsigned char>(0x80 | ((code_point >> 12) & 0x3F));
            bytes[count++] = static_cast<unsigned char>(0x80 | ((code_point >> 6) & 0x3F));
            bytes[count++] = static_cast<unsigned char>(0x80 | (code_point & 0x3F));
        }
        for (int k = 0; k < count; ++k) append_escape(sb, bytes[k]);
        i += used;
    }
    return sb;
}

int decode_hex(char16_t c) {
    if ((c >= u'0') && (c <= u'9')) return c - u'0';
    if ((c >= u'a') && (c <= u'f')) return c - u'a' + 10;
    if ((c >= u'A') && (c <= u'F')) return c - u'A' + 10;
    return -1;
}

// The JDK decodes the escaped octets with a REPLACE-mode UTF-8 decoder: one U+FFFD per
// malformed maximal subpart, the rest of the run re-examined.
void decode_utf8_replace(const std::string& bytes, U16& out) {
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        if (b0 < 0x80) {
            out.push_back(static_cast<char16_t>(b0));
            ++i;
            continue;
        }
        std::size_t need = 0;
        unsigned char second_lo = 0x80;
        unsigned char second_hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            need = 2;
        } else if (b0 == 0xE0) {
            need = 3;
            second_lo = 0xA0;
        } else if (b0 >= 0xE1 && b0 <= 0xEC) {
            need = 3;
        } else if (b0 == 0xED) {
            need = 3;
            second_hi = 0x9F;
        } else if (b0 >= 0xEE && b0 <= 0xEF) {
            need = 3;
        } else if (b0 == 0xF0) {
            need = 4;
            second_lo = 0x90;
        } else if (b0 >= 0xF1 && b0 <= 0xF3) {
            need = 4;
        } else if (b0 == 0xF4) {
            need = 4;
            second_hi = 0x8F;
        } else {
            // 80..C1 and F5..FF are never valid leads
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        std::size_t consumed = 1;
        bool complete = false;
        const auto next = [&](std::size_t offset) {
            return static_cast<unsigned char>(bytes[i + offset]);
        };
        if (i + 1 < n && next(1) >= second_lo && next(1) <= second_hi) {
            consumed = 2;
            if (need == 2) {
                complete = true;
            } else if (i + 2 < n && next(2) >= 0x80 && next(2) <= 0xBF) {
                consumed = 3;
                if (need == 3) {
                    complete = true;
                } else if (i + 3 < n && next(3) >= 0x80 && next(3) <= 0xBF) {
                    consumed = 4;
                    complete = true;
                }
            }
        }
        if (complete) {
            std::uint32_t code_point = 0;
            if (need == 2) {
                code_point = ((static_cast<std::uint32_t>(b0) & 0x1F) << 6) |
                             (next(1) & 0x3F);
            } else if (need == 3) {
                code_point = ((static_cast<std::uint32_t>(b0) & 0x0F) << 12) |
                             ((next(1) & 0x3F) << 6) | (next(2) & 0x3F);
            } else {
                code_point = ((static_cast<std::uint32_t>(b0) & 0x07) << 18) |
                             ((next(1) & 0x3F) << 12) | ((next(2) & 0x3F) << 6) |
                             (next(3) & 0x3F);
            }
            if (code_point <= 0xFFFF) {
                out.push_back(static_cast<char16_t>(code_point));
            } else {
                code_point -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
            }
            i += need;
        } else {
            out.push_back(0xFFFD);
            i += consumed;
        }
    }
}

// Evaluates all escapes in s, applying UTF-8 decoding if needed. Assumes that escapes are
// well-formed syntactically, i.e., of the form %XX. If a sequence of escaped octets is not
// valid UTF-8 then the erroneous octets are replaced with '\uFFFD'.
//
// Exception: any "%" found between "[]" is left alone. It is an IPv6 literal with a scope_id
U16 decode_component(const U16& s, bool ignore_percent_in_brackets) {
    const std::size_t n = s.size();
    if (n == 0) return s;
    if (s.find(u'%') == U16::npos) return s;
    U16 out;
    std::string byte_run;
    std::size_t i = 0;
    bool between_brackets = false;
    while (i < n) {
        char16_t c = s[i];
        if (c == u'[') {
            between_brackets = true;
        } else if (between_brackets && c == u']') {
            between_brackets = false;
        }
        if (c != u'%' || (between_brackets && ignore_percent_in_brackets)) {
            out.push_back(c);
            if (++i >= n) break;
            continue;
        }
        if (i + 2 >= n) {
            // Not reachable for parsed components: the parser only admits well-formed pairs.
            out.push_back(c);
            ++i;
            continue;
        }
        byte_run.clear();
        for (;;) {
            const int hi_nibble = decode_hex(s[i + 1]);
            const int lo_nibble = decode_hex(s[i + 2]);
            byte_run.push_back(static_cast<char>(((hi_nibble & 0xF) << 4) | (lo_nibble & 0xF)));
            i += 3;
            if (i >= n) break;
            c = s[i];
            if (c != u'%') break;
        }
        decode_utf8_replace(byte_run, out);
    }
    return out;
}

// -- String construction ----------------------------------------------------------------------

// If a scheme is given then the path, if given, must be absolute
void check_path(const std::optional<U16>& scheme, const std::optional<U16>& path) {
    if (scheme) {
        if (path && !path->empty() && (*path)[0] != u'/') {
            fail("Relative path in absolute URI");
        }
    }
}

void append_authority(U16& sb, const std::optional<U16>& authority,
                      const std::optional<U16>& user_info, const std::optional<U16>& host,
                      int port) {
    if (host) {
        sb.append(u"//");
        if (user_info) {
            sb.append(quote(*user_info, L_USERINFO, H_USERINFO));
            sb.push_back(u'@');
        }
        const bool need_brackets =
            ((host->find(u':') != U16::npos) && (host->front() != u'[') && (host->back() != u']'));
        if (need_brackets) sb.push_back(u'[');
        sb.append(*host);
        if (need_brackets) sb.push_back(u']');
        if (port != -1) {
            sb.push_back(u':');
            const std::string digits = std::to_string(port);
            for (const char digit : digits) {
                sb.push_back(static_cast<char16_t>(static_cast<unsigned char>(digit)));
            }
        }
    } else if (authority) {
        sb.append(u"//");
        if (!authority->empty() && authority->front() == u'[') {
            // authority should (but may not) contain an embedded IPv6 address
            const std::size_t end = authority->find(u']');
            U16 doquote = *authority;
            if (end != U16::npos && authority->find(u':') != U16::npos) {
                // the authority contains an IPv6 address
                sb.append(authority->substr(0, end + 1));
                doquote = authority->substr(end + 1);
            }
            sb.append(quote(doquote, L_REG_NAME | L_SERVER, H_REG_NAME | H_SERVER));
        } else {
            sb.append(quote(*authority, L_REG_NAME | L_SERVER, H_REG_NAME | H_SERVER));
        }
    }
}

void append_scheme_specific_part(U16& sb, const std::optional<U16>& opaque_part,
                                 const std::optional<U16>& authority,
                                 const std::optional<U16>& user_info,
                                 const std::optional<U16>& host, int port,
                                 const std::optional<U16>& path,
                                 const std::optional<U16>& query) {
    if (opaque_part) {
        if (opaque_part->starts_with(u"//[")) {
            const std::size_t end = opaque_part->find(u']');
            if (end != U16::npos && opaque_part->find(u':') != U16::npos) {
                const U16 doquote = opaque_part->substr(end + 1);
                sb.append(opaque_part->substr(0, end + 1));
                sb.append(quote(doquote, L_URIC, H_URIC));
            }
        } else {
            sb.append(quote(*opaque_part, L_URIC, H_URIC));
        }
    } else {
        append_authority(sb, authority, user_info, host, port);
        if (path) sb.append(quote(*path, L_PATH, H_PATH));
        if (query) {
            sb.push_back(u'?');
            sb.append(quote(*query, L_URIC, H_URIC));
        }
    }
}

void append_fragment(U16& sb, const std::optional<U16>& fragment) {
    if (fragment) {
        sb.push_back(u'#');
        sb.append(quote(*fragment, L_URIC, H_URIC));
    }
}

U16 to_string_components(const std::optional<U16>& scheme, const std::optional<U16>& opaque_part,
                         const std::optional<U16>& authority,
                         const std::optional<U16>& user_info, const std::optional<U16>& host,
                         int port, const std::optional<U16>& path,
                         const std::optional<U16>& query, const std::optional<U16>& fragment) {
    U16 sb;
    if (scheme) {
        sb.append(*scheme);
        sb.push_back(u':');
    }
    append_scheme_specific_part(sb, opaque_part, authority, user_info, host, port, path, query);
    append_fragment(sb, fragment);
    return sb;
}

// define_string_utf8 (in the public-API section below) rebuilds the raw text from the raw
// components, exactly like defineString(): components are appended unquoted, only the host
// gets its RFC2732 brackets back if it lost them.

// -- Conversion helpers for the public API ----------------------------------------------------

std::optional<std::string> to_utf8_opt(const std::optional<U16>& value) {
    if (!value) return std::nullopt;
    return utf16::to_utf8(*value);
}

Uri materialize(const Impl& impl, std::string text) {
    Uri uri;
    uri.scheme = to_utf8_opt(impl.scheme);
    uri.fragment = to_utf8_opt(impl.fragment);
    uri.authority = to_utf8_opt(impl.authority);
    uri.user_info = to_utf8_opt(impl.user_info);
    uri.host = to_utf8_opt(impl.host);
    uri.port = impl.port;
    uri.path = to_utf8_opt(impl.path);
    uri.query = to_utf8_opt(impl.query);
    uri.scheme_specific_part = to_utf8_opt(impl.scheme_specific_part);
    uri.text = std::move(text);
    return uri;
}

// defineString() on the public (UTF-8) representation, used after normalize() invalidates the
// cached raw text. Every appended component is raw; only the host gets its brackets back.
std::string define_string_utf8(const Uri& uri) {
    std::string sb;
    if (uri.scheme) {
        sb += *uri.scheme;
        sb += ':';
    }
    if (is_opaque(uri)) {
        if (uri.scheme_specific_part) sb += *uri.scheme_specific_part;
    } else {
        if (uri.host) {
            sb += "//";
            if (uri.user_info) {
                sb += *uri.user_info;
                sb += '@';
            }
            const bool need_brackets = uri.host->find(':') != std::string::npos &&
                                       uri.host->front() != '[' && uri.host->back() != ']';
            if (need_brackets) sb += '[';
            sb += *uri.host;
            if (need_brackets) sb += ']';
            if (uri.port != -1) {
                sb += ':';
                sb += std::to_string(uri.port);
            }
        } else if (uri.authority) {
            sb += "//";
            sb += *uri.authority;
        }
        if (uri.path) sb += *uri.path;
        if (uri.query) {
            sb += '?';
            sb += *uri.query;
        }
    }
    if (uri.fragment) {
        sb += '#';
        sb += *uri.fragment;
    }
    return sb;
}

// --- [append point] ---

}  // namespace

bool is_opaque(const Uri& uri) { return !uri.path.has_value(); }

bool is_absolute(const Uri& uri) { return uri.scheme.has_value(); }

std::optional<Uri> parse(std::string_view input) {
    Impl impl;
    impl.input = utf16::from_utf8(input);
    try {
        parse_uri(impl, false);
    } catch (const SyntaxError&) {
        return std::nullopt;
    }
    return materialize(impl, std::string(input));
}

std::optional<Uri> build(const std::optional<std::string>& scheme,
                         const std::optional<std::string>& user_info,
                         const std::optional<std::string>& host, int port,
                         const std::optional<std::string>& path,
                         const std::optional<std::string>& query,
                         const std::optional<std::string>& fragment) {
    const std::optional<U16> scheme_u16 =
        scheme ? std::optional<U16>(utf16::from_utf8(*scheme)) : std::nullopt;
    const std::optional<U16> user_info_u16 =
        user_info ? std::optional<U16>(utf16::from_utf8(*user_info)) : std::nullopt;
    const std::optional<U16> host_u16 =
        host ? std::optional<U16>(utf16::from_utf8(*host)) : std::nullopt;
    const std::optional<U16> path_u16 =
        path ? std::optional<U16>(utf16::from_utf8(*path)) : std::nullopt;
    const std::optional<U16> query_u16 =
        query ? std::optional<U16>(utf16::from_utf8(*query)) : std::nullopt;
    const std::optional<U16> fragment_u16 =
        fragment ? std::optional<U16>(utf16::from_utf8(*fragment)) : std::nullopt;
    const U16 s = to_string_components(scheme_u16, std::nullopt, std::nullopt, user_info_u16,
                                       host_u16, port, path_u16, query_u16, fragment_u16);
    Impl impl;
    try {
        check_path(scheme_u16, path_u16);
        impl.input = s;
        parse_uri(impl, true);
    } catch (const SyntaxError&) {
        return std::nullopt;
    }
    return materialize(impl, utf16::to_utf8(s));
}

std::string to_string(const Uri& uri) { return uri.text; }

std::string to_ascii_string(const Uri& uri) {
    const U16 text = utf16::from_utf8(uri.text);
    for (const char16_t c : text) {
        if (c >= 0x80) return utf16::to_utf8(encode(text));
    }
    return uri.text;
}

Uri normalize(const Uri& uri) {
    if (is_opaque(uri) || !uri.path || uri.path->empty()) return uri;
    const U16 raw = utf16::from_utf8(*uri.path);
    U16 normalized;
    if (!normalize_path(raw, normalized)) return uri;
    Uri v = uri;
    v.path = utf16::to_utf8(normalized);
    v.text = define_string_utf8(v);
    return v;
}

std::optional<std::string> decoded_authority(const Uri& uri) {
    if (!uri.authority) return std::nullopt;
    return utf16::to_utf8(decode_component(utf16::from_utf8(*uri.authority), true));
}

std::optional<std::string> decoded_user_info(const Uri& uri) {
    if (!uri.user_info) return std::nullopt;
    return utf16::to_utf8(decode_component(utf16::from_utf8(*uri.user_info), true));
}

std::optional<std::string> decoded_path(const Uri& uri) {
    if (!uri.path) return std::nullopt;
    return utf16::to_utf8(decode_component(utf16::from_utf8(*uri.path), true));
}

std::optional<std::string> decoded_query(const Uri& uri) {
    if (!uri.query) return std::nullopt;
    return utf16::to_utf8(decode_component(utf16::from_utf8(*uri.query), false));
}

std::optional<std::string> decoded_fragment(const Uri& uri) {
    if (!uri.fragment) return std::nullopt;
    return utf16::to_utf8(decode_component(utf16::from_utf8(*uri.fragment), false));
}

}  // namespace agora::core::java_uri
