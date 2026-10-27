// Unit tests for the Alpine package core (AlpinePackageIndex.kt + AlpinePackageMetadataStore.kt).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "agora/core/alpine_package_index.hpp"
#include "agora/core/alpine_package_metadata_store.hpp"
#include "agora/core/zip_reader.hpp"
#include "check.hpp"

namespace sandbox = agora::core::sandbox;

namespace {

namespace fs = std::filesystem;

// ── Fixtures ────────────────────────────────────────────────────────────────────

std::filesystem::path temp_root(const std::string& label) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    const auto base = fs::temp_directory_path() /
                      ("agora_alpine_" + label + "_" + std::to_string(dis(gen)));
    fs::create_directories(base);
    return base;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** gzip member wrapping [data] in a single stored (BTYPE=00) DEFLATE block. */
std::string gzip_store(const std::string& data) {
    std::string out;
    out += '\x1F';
    out += '\x8B';
    out += '\x08';  // CM = deflate
    out += '\x00';  // FLG
    out += std::string(4, '\0');  // MTIME
    out += '\x00';  // XFL
    out += '\x03';  // OS = Unix
    out += '\x01';  // BFINAL=1, BTYPE=00 (stored), byte-aligned
    const auto len = static_cast<std::uint16_t>(data.size());
    const auto nlen = static_cast<std::uint16_t>(~len);
    out += static_cast<char>(len & 0xFF);
    out += static_cast<char>((len >> 8) & 0xFF);
    out += static_cast<char>(nlen & 0xFF);
    out += static_cast<char>((nlen >> 8) & 0xFF);
    out += data;
    const std::uint32_t crc = agora::core::zip::crc32(data);
    const std::uint32_t isize = static_cast<std::uint32_t>(data.size());
    for (int shift = 0; shift < 32; shift += 8) {
        out += static_cast<char>((crc >> shift) & 0xFF);
    }
    for (int shift = 0; shift < 32; shift += 8) {
        out += static_cast<char>((isize >> shift) & 0xFF);
    }
    return out;
}

/** One ustar tar member (checksum left zeroed — the reader does not verify it). */
std::string tar_member(const std::string& name, const std::string& data) {
    std::string header(512, '\0');
    name.copy(header.data(), std::min<std::size_t>(name.size(), 100));
    char size_field[12];
    std::snprintf(size_field, sizeof(size_field), "%011o", static_cast<unsigned>(data.size()));
    for (int i = 0; i < 11; ++i) header[124 + i] = size_field[i];
    header[156] = '0';  // regular file
    static const char kMagic[6] = {'u', 's', 't', 'a', 'r', '\0'};
    std::copy(std::begin(kMagic), std::end(kMagic), header.begin() + 257);
    header[263] = '0';
    header[264] = '0';

    std::string out = header + data;
    const std::size_t padded = ((data.size() + 511) / 512) * 512;
    out.append(padded - data.size(), '\0');
    return out;
}

// ── compareAlpineVersions ───────────────────────────────────────────────────────

TEST(alpine_version_revision_order) {
    CHECK(sandbox::compare_alpine_versions("3.5.2-r1", "3.5.2-r0") > 0);
    CHECK(sandbox::compare_alpine_versions("1.0-r10", "1.0-r2") > 0);
    CHECK(sandbox::compare_alpine_versions("3.5.2", "3.5.2-r0") == 0);
    CHECK(sandbox::compare_alpine_versions("3.5.2-r1", "3.5.2-r1") == 0);
    CHECK(sandbox::compare_alpine_versions("3.5.2-r0", "3.5.2-r1") < 0);
}

TEST(alpine_version_numeric_tokens_compare_numerically) {
    CHECK(sandbox::compare_alpine_versions("1.10", "1.9") > 0);
    CHECK(sandbox::compare_alpine_versions("1.2.3", "1.10.0") < 0);
    CHECK(sandbox::compare_alpine_versions("2", "10") < 0);
}

TEST(alpine_version_prerelease_tokens_sort_before_release) {
    // '_' has lower priority than everything except another '_'.
    CHECK(sandbox::compare_alpine_versions("1.2.3_pre1-r0", "1.2.3-r0") < 0);
    // '~' as its own token carries weight −1; alpha/beta/rc words their own weights.
    CHECK(sandbox::compare_alpine_versions("1.0~1", "1.0") < 0);
    CHECK(sandbox::compare_alpine_versions("1.0alpha1", "1.0") < 0);
    CHECK(sandbox::compare_alpine_versions("1.0alpha1", "1.0beta1") < 0);
    CHECK(sandbox::compare_alpine_versions("1.0beta1", "1.0rc1") < 0);
    CHECK(sandbox::compare_alpine_versions("1.0rc1", "1.0rc2") < 0);
    CHECK(sandbox::compare_alpine_versions("1.0rc2", "1.0rc1") > 0);
    // Kotlin quirk, pinned: a letter RUN like "~alpha" is one token of weight 0, so it
    // compares lexicographically against the missing token and sorts after the release.
    CHECK(sandbox::compare_alpine_versions("1.0~alpha1", "1.0") > 0);
}

TEST(alpine_version_letter_tokens_compare_lexicographically) {
    CHECK(sandbox::compare_alpine_versions("1.0a", "1.0b") < 0);
    CHECK(sandbox::compare_alpine_versions("1.0b", "1.0a") > 0);
    CHECK(sandbox::compare_alpine_versions("1.0a", "1.0a") == 0);
}

TEST(alpine_version_overflowing_digits_fall_back_to_lexicographic) {
    // Kotlin toIntOrNull overflows → null → the token compares as a letter token.
    CHECK(sandbox::compare_alpine_versions("99999999999999999999", "1") > 0);
}

// ── apkIndexWords ───────────────────────────────────────────────────────────────

TEST(alpine_apk_index_words) {
    const auto words = sandbox::apk_index_words("  so:libc.musl  so:ldd  \t busybox  ");
    CHECK_EQ(std::size_t(3), words.size());
    CHECK_EQ(std::string("so:libc.musl"), words[0]);
    CHECK_EQ(std::string("so:ldd"), words[1]);
    CHECK_EQ(std::string("busybox"), words[2]);
    CHECK(sandbox::apk_index_words("").empty());
    CHECK(sandbox::apk_index_words("   \t ").empty());
}

// ── parseFullApkIndex ───────────────────────────────────────────────────────────

namespace {

const char* kIndexText =
    "P:zlib\n"
    "V:1.2.13-r0\n"
    "p:so:libz.so.1=1.2.13-r0\n"
    "D:\n"
    "k:0\n"
    "\n"
    "P:so:libz.so.1\n"
    "V:1.2.13-r0\n"
    "p:so:libz.so.1=1.2.13-r0\n"
    "\n"
    "P:busybox\n"
    "V:1.36.1-r5\n"
    "p:\n"
    "D:so:libc.musl-aarch64.so.1\n"
    "k:5\n"
    "\n"
    "P:aaa-helper\n"
    "V:2.0.0-r0\n"
    "p:so:libhelper.so.1=2.0.0\n"
    "D:zlib\n"
    "k:1\n"
    "\n";

}  // namespace

TEST(alpine_parse_records_and_packages) {
    const sandbox::ApkIndex index = sandbox::parse_full_apk_index(kIndexText);
    CHECK_EQ(std::size_t(3), index.packages.size());  // so: records are not packages
    CHECK(index.packages.count("zlib") == 1);
    CHECK(index.packages.count("busybox") == 1);
    CHECK(index.packages.count("aaa-helper") == 1);
    CHECK_EQ(std::string("1.2.13-r0"), index.packages.at("zlib").version);
    CHECK_EQ(std::string("1.36.1-r5"), index.packages.at("busybox").version);
    CHECK_EQ(std::size_t(1), index.packages.at("busybox").deps.size());
    CHECK_EQ(std::string("so:libc.musl-aarch64.so.1"), index.packages.at("busybox").deps[0]);
}

TEST(alpine_parse_so_map_prefers_installed_package) {
    // Without a preferred package the raw so: record keeps the slot for a lexicographically
    // smaller provider name; marking the provider installed makes the resolution take over.
    const sandbox::ApkIndex plain = sandbox::parse_full_apk_index(kIndexText);
    CHECK(plain.so_to_pkg.count("so:libz.so.1") == 1);

    const sandbox::ApkIndex preferred = sandbox::parse_full_apk_index(
        kIndexText, std::set<std::string>{"zlib"});
    CHECK_EQ(std::string("zlib"), preferred.so_to_pkg.at("so:libz.so.1"));
    // Alias pins are stripped: substringBefore('=')
    CHECK_EQ(std::string("aaa-helper"), preferred.so_to_pkg.at("so:libhelper.so.1"));
}

TEST(alpine_parse_priority_and_lexicographic_tiebreak) {
    const std::string text =
        "P:zzz-provider\nV:1.0-r0\np:so:liby.so.1=1.0\nk:5\n"
        "\n"
        "P:aaa-provider\nV:1.0-r0\np:so:liby.so.1=1.0\nk:1\n"
        "\n";
    const sandbox::ApkIndex index = sandbox::parse_full_apk_index(text);
    // Higher k: priority wins regardless of name order.
    CHECK_EQ(std::string("zzz-provider"), index.so_to_pkg.at("so:liby.so.1"));

    const std::string tied =
        "P:bbb-provider\nV:1.0-r0\np:so:libz.so.1=1.0\n"
        "\n"
        "P:aaa-provider\nV:1.0-r0\np:so:libz.so.1=1.0\n"
        "\n";
    const sandbox::ApkIndex tie = sandbox::parse_full_apk_index(tied);
    // Equal preference and priority → lexicographically smaller name wins.
    CHECK_EQ(std::string("aaa-provider"), tie.so_to_pkg.at("so:libz.so.1"));
}

TEST(alpine_parse_missing_version_skips_record) {
    const sandbox::ApkIndex index = sandbox::parse_full_apk_index("P:zlib\np:x\n\n");
    CHECK(index.packages.empty());
}

// ── gzip + tar path ─────────────────────────────────────────────────────────────

TEST(alpine_parse_gzip_tar_fixture) {
    const auto base = temp_root("gzip");
    const std::string tar = tar_member("APKINDEX", kIndexText) + std::string(1024, '\0');
    const auto path = base / "APKINDEX.tar.gz";
    write_file(path, gzip_store(tar));

    const sandbox::ApkIndex index =
        sandbox::parse_full_apk_index_gzip(path, std::set<std::string>{"zlib"});
    CHECK_EQ(std::size_t(3), index.packages.size());
    CHECK(index.packages.count("zlib") == 1);
    CHECK(index.packages.count("busybox") == 1);
    CHECK_EQ(std::string("zlib"), index.so_to_pkg.at("so:libz.so.1"));

    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_parse_gzip_rejects_corrupt_stream) {
    const auto base = temp_root("gzipbad");
    const auto path = base / "bad.tar.gz";
    write_file(path, std::string("\x1F\x8B\x08\x00not-a-gzip-payload", 24));
    bool threw = false;
    try {
        sandbox::parse_full_apk_index_gzip(path);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    std::error_code ec;
    fs::remove_all(base, ec);
}

// ── collectAlpinePackageChanges ─────────────────────────────────────────────────

TEST(alpine_collect_changes_walks_deps_and_never_downgrades) {
    std::map<std::string, sandbox::FullPkgEntry> packages;
    packages["target"] = {"target", "2.0-r0", {"so:libc>=1", "helper"}};
    packages["helper"] = {"helper", "9.9-r0", {}};
    packages["musl"] = {"musl", "1.2.5-r0", {}};
    const std::map<std::string, std::string> providers{{"so:libc", "musl"}};
    const std::map<std::string, std::string> installed{
        {"target", "1.0-r0"}, {"helper", "1.0-r0"}, {"musl", "1.2.5-r0"}};

    const auto changes =
        sandbox::collect_alpine_package_changes({"target"}, packages, providers, installed);
    // target is newer → change; helper is newer → change; musl is up to date → skipped.
    // Visit order: target, musl (via so:libc), helper.
    CHECK_EQ(std::size_t(2), changes.size());
    CHECK_EQ(std::string("target"), changes[0]);
    CHECK_EQ(std::string("helper"), changes[1]);
}

TEST(alpine_collect_changes_adds_missing_and_skips_unknown) {
    const std::map<std::string, sandbox::FullPkgEntry> packages{
        {"fresh", {"fresh", "1.0-r0", {}}}};
    const auto changes =
        sandbox::collect_alpine_package_changes({"fresh", "ghost"}, packages, {}, {});
    CHECK_EQ(std::size_t(1), changes.size());
    CHECK_EQ(std::string("fresh"), changes[0]);
}

TEST(alpine_collect_changes_guards_dependency_cycles) {
    std::map<std::string, sandbox::FullPkgEntry> packages;
    packages["a"] = {"a", "1.0-r0", {"b"}};
    packages["b"] = {"b", "1.0-r0", {"a"}};
    const auto changes = sandbox::collect_alpine_package_changes({"a"}, packages, {}, {});
    CHECK_EQ(std::size_t(2), changes.size());
    CHECK_EQ(std::string("a"), changes[0]);
    CHECK_EQ(std::string("b"), changes[1]);
}

TEST(alpine_collect_changes_cuts_dependency_pins) {
    std::map<std::string, sandbox::FullPkgEntry> packages;
    packages["top"] = {"top", "1.0-r0", {"dep=2.0-r0", "other>3", "third<4", "fourth~5"}};
    packages["dep"] = {"dep", "2.0-r0", {}};
    packages["other"] = {"other", "3.0-r0", {}};
    packages["third"] = {"third", "4.0-r0", {}};
    packages["fourth"] = {"fourth", "5.0-r0", {}};
    const auto changes = sandbox::collect_alpine_package_changes({"top"}, packages, {}, {});
    CHECK_EQ(std::size_t(5), changes.size());
}

// ── AlpinePackageMetadataStore ──────────────────────────────────────────────────

TEST(alpine_metadata_sanitize_package_name) {
    CHECK_EQ(std::string("zlib"), sandbox::AlpinePackageMetadataStore::sanitize_package_name(" zlib "));
    CHECK_EQ(std::string("g++"), sandbox::AlpinePackageMetadataStore::sanitize_package_name("g++"));
    bool threw = false;
    try {
        sandbox::AlpinePackageMetadataStore::sanitize_package_name("bad name!");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        sandbox::AlpinePackageMetadataStore::sanitize_package_name("-lead");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(alpine_metadata_world_package_name) {
    CHECK_EQ(std::string("zlib"), sandbox::world_package_name("zlib=1.2.13-r0"));
    CHECK_EQ(std::string("zlib"), sandbox::world_package_name("!zlib<2"));
    CHECK_EQ(std::string("so:libz.so.1"), sandbox::world_package_name(" so:libz.so.1~1 "));
}

TEST(alpine_metadata_read_installed_versions) {
    const auto base = temp_root("installed");
    write_file(base / "lib/apk/db/installed",
               "P:zlib\nV:1.2.13-r0\nA:x86_64\n"
               "\n"
               "P:busybox\nV:1.36.1-r5\nT:Shared box\n"
               "\n"
               "P:zlib\nV:1.3.0-r0\n"
               "\n");
    sandbox::AlpinePackageMetadataStore store(base);
    const auto installed = store.read_installed_versions();
    // Map-put semantics: the repeated name keeps its first position with the later value.
    CHECK_EQ(std::size_t(2), installed.size());
    CHECK_EQ(std::string("zlib"), installed[0].first);
    CHECK_EQ(std::string("1.3.0-r0"), installed[0].second);
    CHECK_EQ(std::string("busybox"), installed[1].first);
    CHECK_EQ(std::string("1.36.1-r5"), installed[1].second);
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_metadata_world_dedupe_and_order) {
    const auto base = temp_root("world");
    write_file(base / "etc/apk/world", "zlib\ncurl\nzlib\ncurl\n\n alpine-baselayout \n");
    sandbox::AlpinePackageMetadataStore store(base);
    const auto world = store.read_world_lines();
    CHECK_EQ(std::size_t(3), world.size());
    CHECK_EQ(std::string("zlib"), world[0]);
    CHECK_EQ(std::string("curl"), world[1]);
    CHECK_EQ(std::string("alpine-baselayout"), world[2]);
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_metadata_capture_base_world_forces_from_world) {
    const auto base = temp_root("baseworld");
    write_file(base / "etc/apk/world", "zlib\ncurl\n");
    sandbox::AlpinePackageMetadataStore store(base);
    store.capture_base_world(true);
    CHECK_EQ(std::string("zlib\ncurl\n"), read_file(base / "etc/agora/base-world"));
    // Non-forced capture must not overwrite the snapshot.
    write_file(base / "etc/apk/world", "other\n");
    store.capture_base_world(false);
    CHECK_EQ(std::string("zlib\ncurl\n"), read_file(base / "etc/agora/base-world"));
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_metadata_capture_base_world_infers_defaults) {
    const auto base = temp_root("baseworld2");
    write_file(base / "lib/apk/db/installed",
               "P:alpine-baselayout\nV:2.14.1-r0\n\nP:busybox\nV:1.36.1-r5\n\nP:curl\nV:8.5.0-r0\n\n");
    write_file(base / "etc/apk/world", "alpine-baselayout\nbusybox\ncurl\n");
    sandbox::AlpinePackageMetadataStore store(base);
    store.capture_base_world(false);
    // Inferred: world lines that are default packages AND installed (keeps world order).
    CHECK_EQ(std::string("alpine-baselayout\nbusybox\n"), read_file(base / "etc/agora/base-world"));
    CHECK(store.is_base_package("alpine-baselayout"));
    CHECK(store.is_base_package("busybox"));
    CHECK(!store.is_base_package("curl"));
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_metadata_ensure_migrates_world_to_explicit) {
    const auto base = temp_root("migrate");
    write_file(base / "lib/apk/db/installed",
               "P:alpine-baselayout\nV:2.14.1-r0\n\nP:zlib\nV:1.2.13-r0\n\nP:curl\nV:8.5.0-r0\n\n");
    write_file(base / "etc/apk/world", "alpine-baselayout\nzlib\n");
    sandbox::AlpinePackageMetadataStore store(base);
    store.ensure_package_metadata();
    // Base world inferred from defaults ∩ installed; explicit = world − base.
    CHECK_EQ(std::string("zlib\n"), read_file(base / "etc/agora/explicit-packages"));
    // Normalized world: base lines first, then explicit.
    CHECK_EQ(std::string("alpine-baselayout\nzlib\n"), read_file(base / "etc/apk/world"));
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_metadata_add_explicit_package_normalizes_world) {
    const auto base = temp_root("addexplicit");
    write_file(base / "lib/apk/db/installed", "P:alpine-baselayout\nV:2.14.1-r0\n\n");
    write_file(base / "etc/apk/world", "alpine-baselayout\n");
    sandbox::AlpinePackageMetadataStore store(base);
    store.add_explicit_package("curl");
    CHECK_EQ(std::string("curl\n"), read_file(base / "etc/agora/explicit-packages"));
    CHECK_EQ(std::string("alpine-baselayout\ncurl\n"), read_file(base / "etc/apk/world"));
    // Adding a base package must not duplicate it in world.
    store.add_explicit_package("alpine-baselayout");
    CHECK_EQ(std::string("alpine-baselayout\ncurl\n"), read_file(base / "etc/apk/world"));
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(alpine_metadata_write_explicit_drops_invalid_entries) {
    const auto base = temp_root("explicitbad");
    write_file(base / "lib/apk/db/installed", "");
    sandbox::AlpinePackageMetadataStore store(base);
    store.write_explicit_packages({"zlib", "bad name", "curl"});
    CHECK_EQ(std::string("zlib\ncurl\n"), read_file(base / "etc/agora/explicit-packages"));
    std::error_code ec;
    fs::remove_all(base, ec);
}
}  // namespace