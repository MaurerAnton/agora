/**
 * agora_utf8_host_test - C++ port of scripts/test-native-utf8.py.
 *
 * Compiles the production UTF-8 helper (scripts/native-tests/native_utf8_probe.cpp, the same
 * sources app/src/main/cpp/jni_utf8.h ships) into a JNI library, runs the probe inside a real
 * JVM with -Xcheck:jni, and prints its JSON report. Requires a JDK (JAVA_HOME or java on PATH)
 * and a host GCC-compatible compiler (CXX or --compiler). Runs on Windows and Linux without
 * Android, a model, Gradle, or a phone - like the python original.
 *
 * Deviations from the python original, both cosmetic: the report is printed/written as compact
 * JSON (python used indent=2) and process output is streamed instead of captured separately.
 *
 * Usage: agora_utf8_host_test [--report out.json] [--compiler /path/to/g++]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "agora/core/json.hpp"

namespace json = ::agora::core::json;

namespace {

struct ProcessResult {
    int exit_code = -1;
    std::string output;  // merged stdout (and, where the platform forces it, stderr)
};

#ifdef _WIN32

/** Windows: build a quoted command line and capture stdout through a temp file. */
ProcessResult run_capture(const std::vector<std::string>& argv, bool merge_stderr) {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path() /
                          ("agora-utf8-host-" + std::to_string(::GetCurrentProcessId()) + ".txt");
    std::string command;
    for (const std::string& arg : argv) {
        if (!command.empty()) {
            command += ' ';
        }
        const bool quote = arg.find(' ') != std::string::npos;
        command += quote ? "\"" + arg + "\"" : arg;
    }
    command += merge_stderr ? " 2>&1" : "";
    command += " > \"" + temp.string() + "\"";
    const int code = std::system(command.c_str());
    std::ifstream in(temp, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    in.close();
    std::error_code ignored;
    fs::remove(temp, ignored);
    ProcessResult result;
    result.exit_code = code == -1 ? -1 : (code >> 8);
    result.output = buffer.str();
    return result;
}

#else

ProcessResult run_capture(const std::vector<std::string>& argv, bool merge_stderr) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        return {-1, {}};
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return {-1, {}};
    }
    if (child == 0) {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        if (merge_stderr) {
            ::dup2(pipe_fds[1], STDERR_FILENO);
        }
        ::close(pipe_fds[1]);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const std::string& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::close(pipe_fds[1]);
    std::string output;
    char buffer[8192];
    for (;;) {
        const ssize_t got = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(pipe_fds[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    ProcessResult result;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.output = output;
    return result;
}

#endif

ProcessResult run(const std::vector<std::string>& argv) { return run_capture(argv, false); }

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

/** PATH lookup, the equivalent of shutil.which(). */
std::string which(const std::string& name) {
#ifdef _WIN32
    const char* path_value = std::getenv("PATH");
    const std::vector<std::string> extensions = {".exe", ".cmd", ".bat", ""};
#else
    const char* path_value = std::getenv("PATH");
    const std::vector<std::string> extensions = {""};
#endif
    if (path_value == nullptr) {
        return {};
    }
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::stringstream paths(path_value);
    std::string directory;
    while (std::getline(paths, directory, separator)) {
        for (const std::string& extension : extensions) {
            const std::filesystem::path candidate =
                std::filesystem::path(directory) / (name + extension);
            if (file_exists(candidate)) {
                return candidate.string();
            }
        }
    }
    return {};
}

/** JAVA_HOME from the environment, else the `java.home` property of the running JVM. */
std::string discover_java_home(std::string& error) {
    if (const char* value = std::getenv("JAVA_HOME"); value != nullptr && *value != '\0') {
        return value;
    }
    const std::string java = which("java");
    if (java.empty()) {
        error = "Set JAVA_HOME or put java on PATH";
        return {};
    }
    const ProcessResult settings = run_capture({java, "-XshowSettings:properties", "-version"},
                                               /*merge_stderr=*/true);
    const std::string marker = "java.home = ";
    std::istringstream lines(settings.output);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t at = line.find(marker);
        if (at == std::string::npos) {
            continue;
        }
        std::string home = line.substr(at + marker.size());
        while (!home.empty() && (home.back() == '\r' || home.back() == ' ')) {
            home.pop_back();
        }
        if (!home.empty()) {
            return home;
        }
    }
    error = "Unable to determine java.home";
    return {};
}

/** Walks up from the executable and the working directory to find the repo root. */
std::string find_root() {
    const std::filesystem::path marker = "scripts/native-tests/native_utf8_probe.cpp";
    std::error_code error;
    const std::filesystem::path exe_dir =
        std::filesystem::absolute(std::filesystem::current_path(), error);
    std::vector<std::filesystem::path> starts = {exe_dir};
    std::error_code link_error;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", link_error);
    if (!link_error) {
        starts.insert(starts.begin(), exe.parent_path());
    }
    for (std::filesystem::path cursor : starts) {
        for (int level = 0; level < 6; ++level) {
            if (file_exists(cursor / marker)) {
                return cursor.string();
            }
            if (!cursor.has_parent_path() || cursor == cursor.parent_path()) {
                break;
            }
            cursor = cursor.parent_path();
        }
    }
    return {};
}

void usage() {
    std::fprintf(stderr,
                 "usage: agora_utf8_host_test [--report out.json] [--compiler g++]\n"
                 "  Set JAVA_HOME or put java on PATH; compiler defaults to $CXX or g++.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string report_path;
    std::string compiler_argument;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--report" && index + 1 < argc) {
            report_path = argv[++index];
        } else if (argument == "--compiler" && index + 1 < argc) {
            compiler_argument = argv[++index];
        } else {
            usage();
            return 2;
        }
    }

    std::string error;
    const std::string java_home = discover_java_home(error);
    if (java_home.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    std::string compiler = compiler_argument;
    if (compiler.empty()) {
        if (const char* value = std::getenv("CXX"); value != nullptr && *value != '\0') {
            compiler = value;
        } else {
            compiler = "g++";
        }
    }
    const std::string compiler_path = which(compiler);
    if (compiler_path.empty()) {
        std::fprintf(stderr, "Set CXX or put a host g++ compiler on PATH\n");
        return 2;
    }
    const std::string root = find_root();
    if (root.empty()) {
        std::fprintf(stderr, "Unable to locate the repository root (scripts/native-tests missing)\n");
        return 2;
    }

#ifdef _WIN32
    const std::string executable = ".exe";
    const std::string library_name = "probe.dll";
    const std::string include_dir = "include/win32";
    const std::string shared_flags = "-static-libgcc -static-libstdc++";
#else
    const std::string executable;
    const std::string library_name = "libprobe.so";
    const std::string include_dir = "include/linux";
    const std::string shared_flags = "-fPIC";
#endif
    const std::filesystem::path root_path(root);
    const std::filesystem::path sources = root_path / "scripts/native-tests";
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("agora-native-utf8-" + std::to_string(
#ifdef _WIN32
                                    ::GetCurrentProcessId()
#else
                                    static_cast<long>(::getpid())
#endif
                                    ));
    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
    std::filesystem::create_directories(output);
    struct OutputGuard {
        std::filesystem::path path;
        ~OutputGuard() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } guard{output};

    const std::filesystem::path library = output / library_name;

    const std::vector<std::string> compile_argv = {
        compiler_path, "-std=c++17", "-shared", "-O2",
    };
    {
        std::istringstream flags(shared_flags);
        std::string flag;
        std::vector<std::string> argv = compile_argv;
        while (flags >> flag) {
            argv.push_back(flag);
        }
        argv.push_back("-I" + (std::filesystem::path(java_home) / "include").string());
        argv.push_back("-I" + (std::filesystem::path(java_home) / include_dir).string());
        argv.push_back("-I" + (root_path / "app/src/main/cpp").string());
        argv.push_back((sources / "native_utf8_probe.cpp").string());
        argv.push_back("-o");
        argv.push_back(library.string());
        const ProcessResult compiled = run(argv);
        if (compiled.exit_code != 0) {
            std::fprintf(stderr, "probe compilation failed (exit %d)\n%s", compiled.exit_code,
                         compiled.output.c_str());
            return 1;
        }
    }

    const ProcessResult javac =
        run({(std::filesystem::path(java_home) / ("bin/javac" + executable)).string(), "--release",
             "17", "-d", output.string(), (sources / "NativeUtf8Probe.java").string()});
    if (javac.exit_code != 0) {
        std::fprintf(stderr, "javac failed (exit %d)\n%s", javac.exit_code, javac.output.c_str());
        return 1;
    }

    const ProcessResult java =
        run({(std::filesystem::path(java_home) / ("bin/java" + executable)).string(),
             "-Xcheck:jni", "--enable-native-access=ALL-UNNAMED", "-cp", output.string(),
             "NativeUtf8Probe", library.string()});
    if (java.exit_code != 0) {
        std::fprintf(stderr, "probe run failed (exit %d)\n%s", java.exit_code,
                     java.output.c_str());
        return 1;
    }

    std::string probe_output = java.output;
    const std::size_t first_brace = probe_output.find('{');
    const std::size_t last_brace = probe_output.rfind('}');
    if (first_brace == std::string::npos || last_brace == std::string::npos ||
        last_brace < first_brace) {
        std::fprintf(stderr, "probe produced no JSON report:\n%s", probe_output.c_str());
        return 1;
    }
    const json::ParseResult parsed =
        json::parse(probe_output.substr(first_brace, last_brace - first_brace + 1));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        std::fprintf(stderr, "probe report is not a JSON object:\n%s", probe_output.c_str());
        return 1;
    }
    json::Value report = *parsed.value;
    // Appended in the python original's order, after the parsed keys.
    json::Member oracle;
    oracle.key = "oracle";
    oracle.value.kind = json::Kind::String;
    oracle.value.text = "Real JVM String.getBytes(StandardCharsets.UTF_8)";
    report.members.push_back(oracle);
    json::Member header;
    header.key = "productionHeader";
    header.value.kind = json::Kind::String;
    header.value.text = "app/src/main/cpp/jni_utf8.h";
    report.members.push_back(header);

    const std::string encoded = json::to_kotlin_string(report);
    if (!report_path.empty()) {
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(report_path).parent_path(), error);
        std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "unable to write %s\n", report_path.c_str());
            return 1;
        }
        out << encoded << "\n";
    }
    std::printf("%s\n", encoded.c_str());
    return 0;
}
