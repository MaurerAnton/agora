// Golden parity test for the sandbox path resolver and file operations.
// Tests the C++ implementation against a controlled temporary directory structure.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/sandbox_path_resolver.hpp"
#include "agora/core/shell_file_edit.hpp"
#include "agora/core/shell_tools.hpp"
#include "check.hpp"

namespace {

namespace sandbox = agora::core::sandbox;
namespace shell_file = agora::core::shell_file;
namespace shell_tools = agora::core::shell_tools;

// Shared test state (thread-local)
static thread_local std::filesystem::path s_rootfs_dir;
static thread_local std::filesystem::path s_home_mount_dir;
static thread_local std::filesystem::path s_shared_storage_dir;
static thread_local std::filesystem::path s_base;

std::filesystem::path get_home_mount_dir() { return s_home_mount_dir; }
std::filesystem::path get_shared_storage_dir() { return s_shared_storage_dir; }
std::filesystem::path get_test_base() { return s_base; }

void cleanup_test_root(const std::filesystem::path& rootfs_dir) {
    std::error_code ec;
    std::filesystem::remove_all(s_base, ec);
}

// Create a test directory structure
std::filesystem::path create_test_root() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    std::filesystem::path base = std::filesystem::temp_directory_path() / ("agora_test_" + std::to_string(dis(gen)));
    std::filesystem::create_directories(base);
    
    // Rootfs directory (separate from mount points)
    std::filesystem::path rootfs_dir = base / "rootfs";
    std::filesystem::create_directories(rootfs_dir);
    
    // Rootfs files
    std::ofstream(rootfs_dir / "test.txt", std::ios::binary) << "Hello World";
    std::ofstream(rootfs_dir / "large.txt", std::ios::binary) << std::string(10000, 'A');
    std::filesystem::create_directories(rootfs_dir / "dir" / "sub");
    std::ofstream(rootfs_dir / "dir" / "c.txt", std::ios::binary) << "content c";
    std::ofstream(rootfs_dir / "dir" / "sub" / "d.txt", std::ios::binary) << "content d";
    std::filesystem::create_directories(rootfs_dir / "other");
    std::ofstream(rootfs_dir / "other" / "e.txt", std::ios::binary) << "content e";
    
    // Home mount directory (outside rootfs)
    std::filesystem::path home_mount_dir = base / "home_mount";
    std::filesystem::create_directories(home_mount_dir / "dir");
    std::ofstream(home_mount_dir / "a.txt", std::ios::binary) << "home a";
    std::ofstream(home_mount_dir / "dir" / "b.txt", std::ios::binary) << "home b";
    
    // Shared storage directory (outside rootfs)
    std::filesystem::path shared_storage_dir = base / "shared_storage";
    std::filesystem::create_directories(shared_storage_dir / "dir");
    std::ofstream(shared_storage_dir / "a.txt", std::ios::binary) << "shared a";
    std::ofstream(shared_storage_dir / "dir" / "b.txt", std::ios::binary) << "shared b";
    
    s_rootfs_dir = rootfs_dir;
    s_home_mount_dir = home_mount_dir;
    s_shared_storage_dir = shared_storage_dir;
    s_base = base;
    
    return rootfs_dir;
}

}  // namespace

TEST(sandbox_path_resolver_resolve) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    
    // Test rootfs resolution
    auto r1 = resolver.resolve_sandbox_path("foo.txt");
    CHECK(r1.virtual_root == "/");
    CHECK(r1.file.filename() == "foo.txt");
    CHECK(r1.physical_root == std::filesystem::canonical(rootfs_dir));
    
    auto r2 = resolver.resolve_sandbox_path("/bar.txt");
    CHECK(r2.virtual_root == "/");
    CHECK(r2.file.filename() == "bar.txt");
    
    auto r3 = resolver.resolve_sandbox_path("dir/baz.txt");
    CHECK(r3.virtual_root == "/");
    CHECK(r3.file.filename() == "baz.txt");
    
    // Test home mount resolution
    auto r4 = resolver.resolve_sandbox_path("/mnt/home/a.txt");
    CHECK(r4.virtual_root == "/mnt/home");
    CHECK(r4.file.filename() == "a.txt");
    CHECK(r4.physical_root == std::filesystem::canonical(home_mount_dir));
    
    auto r5 = resolver.resolve_sandbox_path("/mnt/home/dir/b.txt");
    CHECK(r5.virtual_root == "/mnt/home");
    CHECK(r5.file.filename() == "b.txt");
    
    // Test shared storage resolution
    auto r6 = resolver.resolve_sandbox_path("/mnt/shared/a.txt");
    CHECK(r6.virtual_root == "/mnt/shared");
    CHECK(r6.file.filename() == "a.txt");
    CHECK(r6.physical_root == std::filesystem::canonical(shared_storage_dir));
    
    auto r7 = resolver.resolve_sandbox_path("/mnt/shared/dir/b.txt");
    CHECK(r7.virtual_root == "/mnt/shared");
    CHECK(r7.file.filename() == "b.txt");
    
    // Test normalization
    auto r8 = resolver.resolve_sandbox_path("foo\\bar.txt");
    CHECK(r8.file.filename() == "bar.txt");
    
    auto r9 = resolver.resolve_sandbox_path("/foo//bar///baz.txt");
    CHECK(r9.file.filename() == "baz.txt");
    
    // Test traversal rejection
    bool caught = false;
    try {
        resolver.resolve_sandbox_path("../../../etc/passwd");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("Path traversal") != std::string::npos);
        caught = true;
    }
    CHECK(caught);
    
    caught = false;
    try {
        resolver.resolve_sandbox_path("/mnt/home/../../../etc/passwd");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("Path traversal") != std::string::npos);
        caught = true;
    }
    CHECK(caught);
    
    cleanup_test_root(rootfs_dir);
}

TEST(sandbox_path_resolver_walk) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    
    // Walk rootfs
    std::vector<std::string> files;
    resolver.walk_virtual_files(rootfs_dir, files,
                                std::filesystem::canonical(rootfs_dir).string(),
                                "/", -1);
    std::sort(files.begin(), files.end());
    std::vector<std::string> actual_created = {
        "/test.txt", "/large.txt", "/dir/c.txt", "/dir/sub/d.txt", "/other/e.txt"
    };
    std::sort(actual_created.begin(), actual_created.end());
    CHECK(files == actual_created);
    
    // Walk with depth limit
    files.clear();
    resolver.walk_virtual_files(rootfs_dir, files,
                                std::filesystem::canonical(rootfs_dir).string(),
                                "/", 1);
    std::sort(files.begin(), files.end());
    // Depth 1 means only files directly in root
    std::vector<std::string> depth1_expected = {"/test.txt", "/large.txt"};
    std::sort(depth1_expected.begin(), depth1_expected.end());
    CHECK(files == depth1_expected);
    
    // Walk home mount
    files.clear();
    resolver.walk_virtual_files(home_mount_dir, files,
                                std::filesystem::canonical(home_mount_dir).string(),
                                "/mnt/home", -1);
    std::sort(files.begin(), files.end());
    std::vector<std::string> home_expected = {"/mnt/home/a.txt", "/mnt/home/dir/b.txt"};
    std::sort(home_expected.begin(), home_expected.end());
    CHECK(files == home_expected);
    
    cleanup_test_root(rootfs_dir);
}

TEST(sandbox_file_operations_read) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    std::mutex mutation_mutex;
    
    // Read simple file
    auto r1 = sandbox::file_operations::file_read(resolver, "test.txt", 0, 0);
    CHECK(r1.content == "Hello World");
    CHECK(r1.lines == 1);
    CHECK(r1.total_lines == 1);
    CHECK(r1.total_bytes == 11);
    CHECK(r1.returned_bytes == 11);
    CHECK(r1.offset == 0);
    CHECK(r1.limit == 1048576);  // SHELL_FILE_READ_MAX_BYTES
    CHECK(!r1.truncated);
    
    // Read with offset
    auto r2 = sandbox::file_operations::file_read(resolver, "test.txt", 6, 0);
    CHECK(r2.content == "World");
    CHECK(r2.offset == 6);
    
    // Read with limit
    auto r3 = sandbox::file_operations::file_read(resolver, "test.txt", 0, 5);
    CHECK(r3.content == "Hello");
    CHECK(r3.limit == 5);
    CHECK(r3.returned_bytes == 5);
    
    // Read truncated
    auto r4 = sandbox::file_operations::file_read(resolver, "large.txt", 0, 100);
    CHECK(r4.content.size() == 100);
    CHECK(r4.truncated);
    CHECK(r4.total_bytes == 10000);
    CHECK(r4.total_lines == 1);  // large.txt has no newlines, so 1 line
    
    cleanup_test_root(rootfs_dir);
}

TEST(sandbox_file_operations_write) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    std::mutex mutation_mutex;
    
    // Write new file
    auto err = sandbox::file_operations::file_write(resolver, "new.txt", "New content", mutation_mutex);
    CHECK(!err);
    
    // Verify content
    std::ifstream f(rootfs_dir / "new.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content == "New content");
    
    // Write to nested path (creates parent dirs)
    err = sandbox::file_operations::file_write(resolver, "dir/nested.txt", "Nested", mutation_mutex);
    CHECK(!err);
    f = std::ifstream(rootfs_dir / "dir" / "nested.txt", std::ios::binary);
    content.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content == "Nested");
    
    // Overwrite existing
    err = sandbox::file_operations::file_write(resolver, "test.txt", "Overwritten", mutation_mutex);
    CHECK(!err);
    f = std::ifstream(rootfs_dir / "test.txt", std::ios::binary);
    content.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content == "Overwritten");
    
    // Size limit
    err = sandbox::file_operations::file_write(resolver, "big.txt", std::string(2000000, 'X'), mutation_mutex);
    CHECK(err);
    CHECK(err->find("exceeds 1MB") != std::string::npos);
    
    cleanup_test_root(rootfs_dir);
}

TEST(sandbox_file_operations_glob) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    
    // Glob *.txt in root (matches all .txt files recursively)
    auto [files1, trunc1] = sandbox::file_operations::file_glob(resolver, "*.txt", "/", home_mount_path, std::nullopt);
    std::sort(files1.begin(), files1.end());
    std::vector<std::string> expected1 = {
        "/dir/c.txt", "/dir/sub/d.txt", "/large.txt", "/other/e.txt", "/test.txt"
    };
    CHECK(files1 == expected1);
    CHECK(!trunc1);
    
    // Glob **/*.txt (same as *.txt due to pattern adjustment)
    auto [files2, trunc2] = sandbox::file_operations::file_glob(resolver, "**/*.txt", "/", home_mount_path, std::nullopt);
    std::sort(files2.begin(), files2.end());
    std::vector<std::string> expected2 = {
        "/dir/c.txt", "/dir/sub/d.txt", "/large.txt", "/other/e.txt", "/test.txt"
    };
    CHECK(files2 == expected2);
    CHECK(!trunc2);
    
    // Glob with base path (matches all .txt files under dir recursively)
    auto [files3, trunc3] = sandbox::file_operations::file_glob(resolver, "*.txt", "dir", home_mount_path, std::nullopt);
    std::sort(files3.begin(), files3.end());
    std::vector<std::string> expected3 = {"/dir/c.txt", "/dir/sub/d.txt"};
    CHECK(files3 == expected3);
    
    cleanup_test_root(rootfs_dir);
}

TEST(sandbox_file_operations_grep) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    
    // Grep for "content" in all files
    auto result = sandbox::file_operations::file_grep(resolver, "content", "/", home_mount_path, "");
    CHECK(result.has_value());
    auto [matches, trunc] = *result;
    CHECK(matches.size() == 3);  // c.txt, d.txt, e.txt
    CHECK(!trunc);
    for (const auto& m : matches) {
        CHECK(m.content.find("content") != std::string::npos);
    }
    
    // Grep with file glob filter (dir/*.txt matches only dir/c.txt, not dir/sub/d.txt)
    auto result2 = sandbox::file_operations::file_grep(resolver, "content", "/", home_mount_path, "dir/*.txt");
    CHECK(result2.has_value());
    auto [matches2, trunc2] = *result2;
    CHECK(matches2.size() == 1);  // only dir/c.txt
    CHECK(matches2[0].path == "/dir/c.txt");
    CHECK(matches2[0].content.find("content") != std::string::npos);
    
    cleanup_test_root(rootfs_dir);
}

TEST(sandbox_file_operations_edit) {
    auto rootfs_dir = create_test_root();
    auto home_mount_dir = get_home_mount_dir();
    auto shared_storage_dir = get_shared_storage_dir();
    const std::string home_mount_path = "/mnt/home";
    const std::string shared_mount_path = "/mnt/shared";
    
    sandbox::SandboxPathResolver resolver(
        rootfs_dir, home_mount_dir, home_mount_path, shared_mount_path,
        [&shared_storage_dir]() -> std::optional<std::filesystem::path> {
            return shared_storage_dir;
        }
    );
    std::mutex mutation_mutex;
    
    // Edit existing file
    auto r1 = sandbox::file_operations::file_edit(resolver, "test.txt", "Hello", "Hi", false, mutation_mutex);
    CHECK(r1.replacements == 1);
    CHECK(!r1.error);
    CHECK(!r1.sha256.empty());
    
    // Verify edit
    std::ifstream f(rootfs_dir / "test.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content == "Hi World");
    
    // Edit not found
    auto r2 = sandbox::file_operations::file_edit(resolver, "test.txt", "NotFound", "X", false, mutation_mutex);
    CHECK(r2.replacements == 0);
    CHECK(r2.error);
    CHECK(r2.error->find("not found") != std::string::npos);
    
    // Edit multiple matches without replace_all
    std::ofstream(rootfs_dir / "multi.txt", std::ios::binary) << "foo bar foo";
    auto r3 = sandbox::file_operations::file_edit(resolver, "multi.txt", "foo", "baz", false, mutation_mutex);
    CHECK(r3.replacements == 0);
    CHECK(r3.error);
    CHECK(r3.error->find("matches") != std::string::npos);
    
    // Edit multiple matches with replace_all
    auto r4 = sandbox::file_operations::file_edit(resolver, "multi.txt", "foo", "baz", true, mutation_mutex);
    CHECK(r4.replacements == 2);
    CHECK(!r4.error);
    f = std::ifstream(rootfs_dir / "multi.txt", std::ios::binary);
    content.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content == "baz bar baz");
    
    cleanup_test_root(rootfs_dir);
}