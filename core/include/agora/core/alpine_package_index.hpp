#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::sandbox {

/** FullPkgEntry: one APKINDEX record (name, version, dependency words). */
struct FullPkgEntry {
    std::string name;
    std::string version;
    std::vector<std::string> deps;
};

/**
 * compare_alpine_versions: Alpine-style version ordering (Kotlin compareAlpineVersions).
 * Format {version}-r{revision} (revision defaults to 0); the version part tokenises into
 * digit runs and letter runs; '~' and pre-release words sort before release; '_' sorts
 * below every token except another '_'. Returns >0, 0, <0.
 */
int compare_alpine_versions(const std::string& a, const std::string& b);

/** apkIndexWords: whitespace split of a D:/p: field, dropping empties. */
std::vector<std::string> apk_index_words(const std::string& value);

/** The parsed APKINDEX: package table plus the soname → provider package resolution. */
struct ApkIndex {
    std::map<std::string, FullPkgEntry> packages;
    std::map<std::string, std::string> so_to_pkg;
};

/**
 * parse_full_apk_index: records of the `APKINDEX` member, then the `p:` provides resolution —
 * preferred (installed) beats unpreferred, then k: priority, then lexicographic name.
 * [apkindex_text] is the decompressed APKINDEX member text.
 */
ApkIndex parse_full_apk_index(std::string_view apkindex_text,
                              const std::set<std::string>& preferred_packages = {});

/**
 * Read and gunzip [index_file] (APKINDEX.tar.gz), extract every `APKINDEX` tar member and
 * run parse_full_apk_index. Throws std::runtime_error on malformed gzip/tar.
 */
ApkIndex parse_full_apk_index_gzip(const std::filesystem::path& index_file,
                                   const std::set<std::string>& preferred_packages = {});

/**
 * collect_alpine_package_changes: the shared download closure for install and upgrade.
 * Walks requested → deps (dependency names cut at = > < ~, falling back to providers/so map)
 * and keeps packages that are missing or strictly newer than the installed version.
 * Never downgrades. Result preserves first-visit order.
 */
std::vector<std::string> collect_alpine_package_changes(
    const std::vector<std::string>& requested,
    const std::map<std::string, FullPkgEntry>& packages,
    const std::map<std::string, std::string>& providers,
    const std::map<std::string, std::string>& installed);

}  // namespace agora::core::sandbox