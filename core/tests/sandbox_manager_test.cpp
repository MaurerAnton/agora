// Unit tests for the ProotSandboxManager transport (availability, execution gate,
// file-op delegation, package metadata guards). proot itself stays behind the executor seam.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "agora/core/sandbox_manager.hpp"
#include "check.hpp"

namespace sandbox = agora::core::sandbox;
namespace backends = agora::core::shell_backends;

namespace {

namespace fs = std::filesystem;

struct Fixture {
    fs::path base;
    fs::path rootfs;
    fs::path home;
};

Fixture temp_fixture() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    Fixture fixture;
    fixture.base =
        fs::temp_directory_path() / ("agora_mgr_" + std::to_string(dis(gen)));
    fixture.rootfs = fixture.base / "rootfs";
    fixture.home = fixture.base / "home";
    fs::create_directories(fixture.rootfs);
    fs::create_directories(fixture.home);
    return fixture;
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

/** rootfs + shell + linker + passwd + home files: the "installed" shape. */
void make_ready_rootfs(const Fixture& fixture) {
    write_file(fixture.rootfs / "bin/sh", "shell");
    write_file(fixture.rootfs / "lib/ld-musl-aarch64.so.1", "linker");
    write_file(fixture.rootfs / "etc/passwd", "root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:x:/nonexistent:/sbin/nologin\n");
    write_file(fixture.home / "a.txt", "home a");
    write_file(fixture.home / "dir/b.txt", "home b hit line");
}

sandbox::ProotSandboxManager::Config make_config(const Fixture& fixture) {
    sandbox::ProotSandboxManager::Config config;
    config.rootfs_dir = fixture.rootfs;
    config.home_mount_dir = fixture.home;
    config.home_mount_path = "/home/agora";
    config.shared_storage_mount_path = "/mnt/shared";
    return config;
}

void cleanup(const Fixture& fixture) {
    std::error_code ec;
    fs::remove_all(fixture.base, ec);
}

}  // namespace

// ── Availability gate ───────────────────────────────────────────────────────────

TEST(sandbox_manager_not_installed_blocks_execution) {
    const Fixture fixture = temp_fixture();
    fs::remove_all(fixture.rootfs);  // no rootfs at all
    int calls = 0;
    auto config = make_config(fixture);
    config.executor = [&](const std::string&, const std::string&, std::int32_t) {
        ++calls;
        return backends::SandboxManager::SandboxResult{"", "", 0, std::nullopt};
    };
    sandbox::ProotSandboxManager manager(std::move(config));
    CHECK(!manager.is_available_sync());
    const auto result = manager.execute_command("ls", "", 1000);
    CHECK_EQ(std::string("Sandbox not installed"), result.stderr_text);
    CHECK_EQ(-1, result.exit_code);
    CHECK_EQ(0, calls);  // the executor must never run for an unavailable sandbox
    CHECK(manager.last_error().find("rootfs not found") == 0);
    cleanup(fixture);
}

TEST(sandbox_manager_missing_linker_blocks_availability) {
    const Fixture fixture = temp_fixture();
    write_file(fixture.rootfs / "bin/sh", "shell");
    auto config = make_config(fixture);
    config.executor = [&](const std::string&, const std::string&, std::int32_t) {
        return backends::SandboxManager::SandboxResult{"ran", "", 0, std::nullopt};
    };
    sandbox::ProotSandboxManager manager(std::move(config));
    CHECK(!manager.is_available());
    CHECK_EQ(std::string("musl linker missing"), manager.last_error());
    cleanup(fixture);
}

TEST(sandbox_manager_prepare_repairs_shell_passwd_and_targets) {
    const Fixture fixture = temp_fixture();
    // Broken shape: busybox instead of /bin/sh, /root home, no mount targets.
    write_file(fixture.rootfs / "bin/busybox", "busybox-payload");
    write_file(fixture.rootfs / "lib/ld-musl-aarch64.so.1", "linker");
    write_file(fixture.rootfs / "etc/passwd",
               "root:x:0:0:root:/root:/bin/sh\ndaemon:x:1:1:daemon:/var/log:/sbin/nologin\n");
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));

    CHECK(manager.is_available());
    // ensureShell: /bin/sh copied from busybox
    CHECK(fs::exists(fixture.rootfs / "bin/sh"));
    CHECK_EQ(std::string("busybox-payload"), read_file(fixture.rootfs / "bin/sh"));
    // ensureRootHome: /root → /home/agora, other lines untouched
    const std::string passwd = read_file(fixture.rootfs / "etc/passwd");
    CHECK(passwd.find("root:x:0:0:root:/home/agora:/bin/sh") != std::string::npos);
    CHECK(passwd.find("daemon:x:1:1:daemon:/var/log") != std::string::npos);
    // Mount targets + package metadata
    CHECK(fs::exists(fixture.rootfs / "home/agora"));
    CHECK(fs::exists(fixture.rootfs / "mnt/shared"));
    CHECK(fs::exists(fixture.rootfs / "etc/agora/base-world"));
    CHECK(fs::exists(fixture.home));
    const std::string passwd_after_first = read_file(fixture.rootfs / "etc/passwd");

    // Idempotence: a second prepare must not rewrite the passwd line again.
    CHECK(manager.is_available());
    CHECK_EQ(passwd_after_first, read_file(fixture.rootfs / "etc/passwd"));
    CHECK(read_file(fixture.rootfs / "etc/passwd")
              .find("root:x:0:0:root:/home/agora:") != std::string::npos);
    CHECK(read_file(fixture.rootfs / "etc/passwd").find("/root:") == std::string::npos);
    cleanup(fixture);
}

TEST(sandbox_manager_is_available_sync_is_fast_and_side_effect_free) {
    const Fixture fixture = temp_fixture();
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));
    CHECK(!manager.is_available_sync());  // no rootfs
    CHECK(!fs::exists(fixture.rootfs / "etc/agora"));  // no repair happened

    write_file(fixture.rootfs / "bin/sh", "shell");
    write_file(fixture.rootfs / "lib/ld-musl-aarch64.so.1", "linker");
    CHECK(manager.is_available_sync());
    // Still no repairs: sync reads never create metadata or mount targets.
    CHECK(!fs::exists(fixture.rootfs / "etc/agora"));
    CHECK(!fs::exists(fixture.rootfs / "home/agora"));
    cleanup(fixture);
}

// ── Execution through the executor seam ─────────────────────────────────────────

TEST(sandbox_manager_execute_command_passes_command_workdir_and_timeout) {
    const Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    std::string captured_command;
    std::string captured_workdir;
    std::int32_t captured_timeout = 0;
    int calls = 0;
    auto config = make_config(fixture);
    config.executor = [&](const std::string& command, const std::string& workdir,
                          std::int32_t timeout) {
        ++calls;
        captured_command = command;
        captured_workdir = workdir;
        captured_timeout = timeout;
        return backends::SandboxManager::SandboxResult{"pkg list", "", 2, std::nullopt};
    };
    sandbox::ProotSandboxManager manager(std::move(config));

    // Blank workdir defaults to the home mount path (Kotlin .ifBlank { homeMountPath }).
    const auto blank_workdir = manager.execute_command("apk list", "", 4321);
    CHECK_EQ(1, calls);
    CHECK_EQ(std::string("apk list"), captured_command);
    CHECK_EQ(std::string("/home/agora"), captured_workdir);
    CHECK_EQ(4321, captured_timeout);
    CHECK_EQ(std::string("pkg list"), blank_workdir.stdout_text);
    CHECK_EQ(2, blank_workdir.exit_code);
    CHECK(!blank_workdir.warning.has_value());

    // A non-blank workdir passes through untouched.
    manager.execute_command("pwd", "/etc", 99);
    CHECK_EQ(std::string("/etc"), captured_workdir);
    CHECK_EQ(99, captured_timeout);
    cleanup(fixture);
}

TEST(sandbox_manager_execute_propagates_timeout_and_warning) {
    const Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    auto config = make_config(fixture);
    config.executor = [&](const std::string&, const std::string&, std::int32_t) {
        return backends::SandboxManager::SandboxResult{
            "partial", "Timed out", -1, "Local Sandbox output was truncated at 1048576 UTF-8 bytes."};
    };
    sandbox::ProotSandboxManager manager(std::move(config));
    const auto result = manager.execute_command("sleep 99", "", 10);
    CHECK_EQ(-1, result.exit_code);
    CHECK_EQ(std::string("Timed out"), result.stderr_text);
    CHECK_EQ(std::string("partial"), result.stdout_text);
    CHECK(result.warning.has_value());
    cleanup(fixture);
}

// ── File operations delegate to the pinned file_operations ──────────────────────

TEST(sandbox_manager_file_read_write_edit) {
    const Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));

    const auto read = manager.file_read("/home/agora/a.txt", 0, 0);
    CHECK_EQ(std::string("home a"), read.content);
    CHECK_EQ(6, read.total_bytes);
    CHECK(!read.error.has_value());

    CHECK(!manager.file_write("/home/agora/w.txt", "payload").has_value());
    CHECK_EQ(std::string("payload"), read_file(fixture.home / "w.txt"));

    const auto edit = manager.file_edit("/home/agora/a.txt", "home", "HOM", false);
    CHECK_EQ(1, edit.replacements);
    CHECK_EQ(std::string("HOM a"), read_file(fixture.home / "a.txt"));

    // A missing old string reports zero replacements, like Kotlin.
    const auto miss = manager.file_edit("/home/agora/a.txt", "absent", "x", false);
    CHECK_EQ(0, miss.replacements);
    cleanup(fixture);
}

TEST(sandbox_manager_file_glob_and_grep) {
    const Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));

    const auto glob = manager.file_glob("*.txt", "", std::nullopt);
    CHECK(glob.second == false);  // not truncated
    bool found_home = false;
    bool found_nested = false;
    for (const auto& file : glob.first) {
        if (file == "/home/agora/a.txt") found_home = true;
        if (file == "/home/agora/dir/b.txt") found_nested = true;
    }
    CHECK(found_home);
    CHECK(found_nested);

    const auto grep = manager.file_grep("hit", "", "");
    CHECK(grep.ok);
    CHECK_EQ(std::size_t(1), grep.matches.size());
    CHECK_EQ(std::string("/home/agora/dir/b.txt"), grep.matches[0].path);
    CHECK_EQ(1, grep.matches[0].line);
    CHECK_EQ(std::string("home b hit line"), grep.matches[0].content);

    // Kotlin Regex(pattern) failure → Result.failure carried as ok=false with a message.
    const auto bad_regex = manager.file_grep("[", "", "");
    CHECK(!bad_regex.ok);
    CHECK(!bad_regex.error.empty());
    cleanup(fixture);
}

TEST(sandbox_manager_file_read_rejects_traversal) {
    const Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    // Symlink escaping the home mount: canonical resolution leaves the physical root.
    write_file(fixture.base / "outside.txt", "secret");
    fs::create_symlink(fixture.base / "outside.txt", fixture.home / "escape.txt");
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));

    bool threw = false;
    std::string message;
    try {
        manager.file_read("/home/agora/escape.txt", 0, 0);
    } catch (const std::exception& error) {
        threw = true;
        message = error.what();
    }
    CHECK(threw);
    CHECK(message.find("Path traversal") != std::string::npos);

    // The same failure surfaces as a grep outcome failure.
    const auto grep = manager.file_grep("x", "/home/agora/escape.txt", "");
    CHECK(!grep.ok);
    CHECK(grep.error.find("Path traversal") != std::string::npos);
    cleanup(fixture);
}

// ── Package management ──────────────────────────────────────────────────────────

TEST(sandbox_manager_apk_list_parses_installed_db) {
    Fixture fixture = temp_fixture();
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager unavailable(std::move(config));
    CHECK(unavailable.apk_list().empty());  // unavailable sandbox → empty list
    cleanup(fixture);

    fixture = temp_fixture();
    make_ready_rootfs(fixture);
    write_file(fixture.rootfs / "lib/apk/db/installed",
               "P:zlib\nV:1.2.13-r0\nT:compression library\n"
               "\n"
               "P:busybox\nV:1.36.1-r5\n"
               "\n"
               "P:curl\nV:8.5.0-r0\nT:transfers URLs\n");  // no trailing blank → final flush
    auto ready_config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(ready_config));
    const auto packages = manager.apk_list();
    CHECK_EQ(std::size_t(3), packages.size());
    CHECK_EQ(std::string("zlib"), packages[0].name);
    CHECK_EQ(std::string("1.2.13-r0"), packages[0].version);
    CHECK_EQ(std::string("compression library"), packages[0].description);
    CHECK_EQ(std::string("busybox"), packages[1].name);
    CHECK(packages[1].description.empty());
    CHECK_EQ(std::string("curl"), packages[2].name);
    CHECK_EQ(std::string("8.5.0-r0"), packages[2].version);
    cleanup(fixture);
}

TEST(sandbox_manager_apk_delete_rejects_invalid_and_base_packages) {
    Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    write_file(fixture.rootfs / "lib/apk/db/installed",
               "P:alpine-baselayout\nV:2.14.1-r0\n\nP:curl\nV:8.5.0-r0\n\n");
    write_file(fixture.rootfs / "etc/apk/world", "alpine-baselayout\ncurl\n");
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));

    // Invalid name → failure with the Kotlin message.
    CHECK(!manager.apk_delete("bad name"));
    CHECK(manager.last_error().find("Invalid package name") == 0);

    // Base package → refused.
    CHECK(!manager.apk_delete("alpine-baselayout"));
    CHECK_EQ(std::string("Refusing to remove base package: alpine-baselayout"),
             manager.last_error());
    cleanup(fixture);
}

TEST(sandbox_manager_apk_delete_cleans_stale_explicit_entry) {
    Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    write_file(fixture.rootfs / "lib/apk/db/installed",
               "P:alpine-baselayout\nV:2.14.1-r0\n\n");
    write_file(fixture.rootfs / "etc/apk/world", "alpine-baselayout\n");
    write_file(fixture.rootfs / "etc/agora/base-world", "alpine-baselayout\n");
    write_file(fixture.rootfs / "etc/agora/explicit-packages", "ghost\n");
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));

    // Not installed → drop the stale explicit entry, re-normalize, succeed.
    CHECK(manager.apk_delete("ghost"));
    CHECK_EQ(std::string(""), read_file(fixture.rootfs / "etc/agora/explicit-packages"));
    CHECK_EQ(std::string("alpine-baselayout\n"), read_file(fixture.rootfs / "etc/apk/world"));
    cleanup(fixture);
}

TEST(sandbox_manager_apk_delete_runs_apk_del_and_verifies) {
    Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    write_file(fixture.rootfs / "lib/apk/db/installed",
               "P:alpine-baselayout\nV:2.14.1-r0\n\nP:curl\nV:8.5.0-r0\n\n");
    write_file(fixture.rootfs / "etc/apk/world", "alpine-baselayout\ncurl\n");

    std::string captured_command;
    int calls = 0;
    auto config = make_config(fixture);
    // Failure path: apk del reports an error and the DB still lists the package.
    config.executor = [&](const std::string& command, const std::string&, std::int32_t) {
        ++calls;
        captured_command = command;
        return backends::SandboxManager::SandboxResult{"", "ERROR: unable to remove", 1,
                                                       std::nullopt};
    };
    sandbox::ProotSandboxManager manager(std::move(config));
    CHECK(!manager.apk_delete("curl"));
    CHECK_EQ(std::string("apk del 'curl'"), captured_command);
    CHECK_EQ(std::string("ERROR: unable to remove"), manager.last_error());
    // The explicit/world state is restored after the failed transaction.
    CHECK_EQ(std::string("curl\n"), read_file(fixture.rootfs / "etc/agora/explicit-packages"));
    CHECK_EQ(std::string("alpine-baselayout\ncurl\n"), read_file(fixture.rootfs / "etc/apk/world"));

    // Success path: the executor rewrites the installed DB, so verification passes.
    const auto installed_db = fixture.rootfs / "lib/apk/db/installed";
    auto success_config = make_config(fixture);
    success_config.executor = [&](const std::string&, const std::string&, std::int32_t) {
        write_file(installed_db, "P:alpine-baselayout\nV:2.14.1-r0\n\n");
        return backends::SandboxManager::SandboxResult{"", "", 0, std::nullopt};
    };
    sandbox::ProotSandboxManager remover(std::move(success_config));
    CHECK(remover.apk_delete("curl"));
    CHECK_EQ(std::string("alpine-baselayout\n"), read_file(fixture.rootfs / "etc/apk/world"));
    CHECK_EQ(std::string(""), read_file(fixture.rootfs / "etc/agora/explicit-packages"));
    cleanup(fixture);
}

// ── Disk usage and reset ────────────────────────────────────────────────────────

TEST(sandbox_manager_disk_usage_in_megabytes) {
    Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    write_file(fixture.rootfs / "big.bin", std::string(1258291, 'a'));  // ~1.2 MiB
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));
    CHECK_EQ(1, manager.disk_usage_mb());
    cleanup(fixture);
}

TEST(sandbox_manager_reset_deletes_rootfs) {
    Fixture fixture = temp_fixture();
    make_ready_rootfs(fixture);
    write_file(fixture.rootfs / "var/cache/apk/db", "x");
    auto config = make_config(fixture);
    sandbox::ProotSandboxManager manager(std::move(config));
    CHECK(manager.reset());
    CHECK(!fs::exists(fixture.rootfs));
    // Home mount dir survives (it is not part of the rootfs).
    CHECK(fs::exists(fixture.home));
    cleanup(fixture);
}