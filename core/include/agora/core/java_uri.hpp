#pragma once

#include <optional>
#include <string>
#include <string_view>

// A C++ port of java.net.URI with the JDK 26 semantics the golden traces were frozen against
// (core/tools/golden/citations). Only the surface the app needs is exposed: parsing, the
// multi-argument constructor, the component getters, toString(), toASCIIString() and
// normalize(). Everything the parser validates (scheme, authority split, IPv4/IPv6/hostname
// scanners, escape pairs) is ported 1:1, malformed input simply makes parse()/build() return
// nullopt where the JDK throws URISyntaxException.
//
// Boundaries (none of them reachable from the frozen corpus):
//  * The JVM NPE paths (encode on unpaired surrogates) cannot be represented: input travels as
//    UTF-8 and lone surrogates are rejected by construction.
//  * Normalizer.normalize(NFC) inside encode() is not ported; all pinned inputs are already
//    NFC-normalized precomposed text.
namespace agora::core::java_uri {

/** A parsed java.net.URI. Component strings are raw (undecoded) exactly like the JDK fields. */
struct Uri {
    std::optional<std::string> scheme;
    std::optional<std::string> fragment;
    std::optional<std::string> authority;             // raw, registry or server form
    std::optional<std::string> user_info;             // raw
    std::optional<std::string> host;                  // raw, an IPv6 literal keeps its brackets
    int port = -1;
    std::optional<std::string> path;                  // raw; nullopt means the URI is opaque
    std::optional<std::string> query;                 // raw
    std::optional<std::string> scheme_specific_part;  // raw, opaque URIs only
    std::string text;                                 // toString(): the input or the rebuilt text
};

/** URI(String) -> parse(false). nullopt stands for the JDK's URISyntaxException. */
std::optional<Uri> parse(std::string_view input);

/** URI(scheme, userInfo, host, port, path, query, fragment): toString(...) + checkPath +
 *  parse(true), as required by CitationPolicy.safeHttpUrl. */
std::optional<Uri> build(const std::optional<std::string>& scheme,
                         const std::optional<std::string>& user_info,
                         const std::optional<std::string>& host,
                         int port,
                         const std::optional<std::string>& path,
                         const std::optional<std::string>& query,
                         const std::optional<std::string>& fragment);

bool is_opaque(const Uri& uri);
bool is_absolute(const Uri& uri);

/** getAuthority(): decoded. */
std::optional<std::string> decoded_authority(const Uri& uri);
/** getUserInfo(): decoded. */
std::optional<std::string> decoded_user_info(const Uri& uri);
/** getPath(): decoded. */
std::optional<std::string> decoded_path(const Uri& uri);
/** getQuery(): decoded, '%' kept inside brackets. */
std::optional<std::string> decoded_query(const Uri& uri);
/** getFragment(): decoded, '%' kept inside brackets. */
std::optional<std::string> decoded_fragment(const Uri& uri);

/** toString(). */
std::string to_string(const Uri& uri);

/** toASCIIString(): percent-encodes every non-ASCII octet. */
std::string to_ascii_string(const Uri& uri);

/** normalize(): returns the same URI when the raw path is already normal, otherwise a copy
 *  with the normalized path whose text is rebuilt from the components. */
Uri normalize(const Uri& uri);

}  // namespace agora::core::java_uri
