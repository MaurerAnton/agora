/**
 * agora_server - C++ replacement for the Python receiver scripts in server/.
 *
 *   agora_server rating            submission-only rating API (env: AGORA_RATING_DB,
 *                                  AGORA_RATING_HOST, AGORA_RATING_PORT)
 *   agora_server crash             anonymous crash receiver (fixed 127.0.0.1:8092, writes
 *                                  /var/lib/agora-crash/crashes.jsonl)
 *   agora_server patch-nginx [f]   idempotently insert the /crash location (default
 *                                  /etc/nginx/sites-available/newoether.com)
 *
 * Wire semantics (status codes, error bodies, CORS, allowlist, rate limit, rotation,
 * timestamps) mirror the Python receivers; see server/*.py in the commit history for the
 * reference implementation this was verified against.
 */
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "agora/server/crash_receiver.hpp"
#include "agora/server/http_server.hpp"
#include "agora/server/patch_nginx.hpp"
#include "agora/server/rating_receiver.hpp"

namespace {

void serve_forever(agora::server::HttpServer& server) {
    server.start();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}

int usage() {
    std::fprintf(stderr,
                 "usage: agora_server <command> [argument]\n"
                 "  rating                      run the rating API (AGORA_RATING_* env)\n"
                 "  crash                       run the crash receiver (127.0.0.1:8092)\n"
                 "  patch-nginx [site-file]     insert the /crash nginx location\n");
    return 1;
}

int patch_nginx_main(const char* path_argument) {
    const std::string path = path_argument != nullptr
                                 ? std::string(path_argument)
                                 : std::string(agora::server::kDefaultNginxSitePath);
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "ERROR: cannot read %s\n", path.c_str());
        return 2;
    }
    std::string text;
    char buffer[8192];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        if (got == 0) {
            break;
        }
        text.append(buffer, got);
    }
    std::fclose(file);

    if (text.find("location = /crash") != std::string::npos) {
        std::puts("already present; no change");
        return 0;
    }
    const std::optional<std::string> patched = agora::server::patch_nginx_text(text);
    if (!patched.has_value()) {
        std::fprintf(stderr, "ERROR: anchor not found\n");
        return 2;
    }
    std::FILE* output = std::fopen(path.c_str(), "wb");
    if (output == nullptr) {
        std::fprintf(stderr, "ERROR: cannot write %s\n", path.c_str());
        return 2;
    }
    std::fwrite(patched->data(), 1, patched->size(), output);
    std::fclose(output);
    std::printf("patched %zu block(s)", agora::server::count_crash_locations(*patched));
    std::fputc('\n', stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    // A client that disconnects mid-response must not kill the service.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    if (argc < 2) {
        return usage();
    }
    const std::string command = argv[1];

    if (command == "rating") {
        const agora::server::RatingConfig config = agora::server::rating_config_from_env();
        const std::filesystem::path parent = std::filesystem::path(config.db_path).parent_path();
        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
        }
        agora::server::HttpServer server(
            config.host, config.port, "agora-rating/1.0",
            [&config](const agora::server::HttpRequest& request) {
                return agora::server::handle_rating_request(request, config);
            });
        serve_forever(server);
    }

    if (command == "crash") {
        // Defaults are the python service constants; the env overrides are test/A-B knobs and
        // keep the deployed behaviour identical when unset.
        agora::server::CrashConfig config{};
        if (const char* host = std::getenv("AGORA_CRASH_HOST"); host != nullptr && *host != '\0') {
            config.host = host;
        }
        if (const char* port = std::getenv("AGORA_CRASH_PORT"); port != nullptr && *port != '\0') {
            config.port = std::atoi(port);
        }
        if (const char* data_dir = std::getenv("AGORA_CRASH_DATA_DIR");
            data_dir != nullptr && *data_dir != '\0') {
            config.data_dir = data_dir;
            config.log_file = (std::filesystem::path(data_dir) / "crashes.jsonl").string();
        }
        if (const char* log_file = std::getenv("AGORA_CRASH_LOG_FILE");
            log_file != nullptr && *log_file != '\0') {
            config.log_file = log_file;
        }
        std::error_code error;
        std::filesystem::create_directories(config.data_dir, error);
        agora::server::CrashReceiver receiver(config);
        agora::server::HttpServer server(config.host, config.port, "agora-crash/1.0",
                                         [&receiver](const agora::server::HttpRequest& request) {
                                             return receiver.handle(request);
                                         });
        serve_forever(server);
    }

    if (command == "patch-nginx") {
        return patch_nginx_main(argc >= 3 ? argv[2] : nullptr);
    }

    return usage();
}
