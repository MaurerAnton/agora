#include "agora/core/shell_tool_backends.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/conch_sse.hpp"
#include "agora/core/crypto_primitives.hpp"
#include "agora/core/json.hpp"
#include "agora/core/portable_glob.hpp"
#include "agora/core/sha256.hpp"
#include "agora/core/unicode.hpp"
#include "utf16.hpp"

namespace agora::core::shell_backends {
namespace {

namespace client = agora::core::shell_client;
namespace json = agora::core::json;

constexpr std::int64_t kFileReadMaxBytes = 1'048'576;
constexpr std::int64_t kFileEditMaxBytes = 16'777'216;
constexpr std::int32_t kFileGlobMaxMatches = 1'000;
constexpr std::int32_t kFileGrepMaxMatches = 500;
constexpr std::int64_t kFileGrepMaxFileBytes = 500LL * 1024LL;
constexpr std::int32_t kFileGrepMaxContentChars = 500;

// ---- JSON builders -------------------------------------------------------------------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value json_bool(bool flag) { return json_literal(flag ? "true" : "false"); }

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

// ---- Kotlin string helpers -----------------------------------------------------------------

/** Kotlin String.trimEnd(vararg chars). */
std::string trim_end_char(std::string_view text, char character) {
    std::size_t end = text.size();
    while (end > 0 && text[end - 1] == character) --end;
    return std::string(text.substr(0, end));
}

/** Kotlin String.trimEnd(): strips trailing characters that isWhitespace() accepts. */
std::string trim_end_whitespace(std::string_view text) {
    const std::u16string units = utf16::from_utf8(text);
    std::size_t end = units.size();
    while (end > 0 &&
           (unicode::flags_of(units[end - 1]) & (unicode::kJvmWhitespace | unicode::kJvmSpace)) !=
               0) {
        --end;
    }
    return utf16::to_utf8(units.substr(0, end));
}

/** `String(bytes, UTF_8)` followed by the JVM's UTF-8 encode: identity on valid input. */
std::string decode_utf8_replace(std::string_view bytes) {
    return utf16::to_utf8(utf16::from_utf8(bytes));
}

/** Kotlin String.lines(): split on CRLF, LF or CR; the empty string yields one empty line. */
std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            lines.push_back(std::move(current));
            current.clear();
        } else if (c == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

/** Kotlin String.substringBeforeLast(delimiter, missingDelimiterValue). */
std::string substring_before_last(std::string_view text, char delimiter) {
    const std::size_t at = text.rfind(delimiter);
    if (at == std::string_view::npos) return "";
    return std::string(text.substr(0, at));
}

/** Kotlin String.toIntOrNull: optional sign, decimal digits, Int range. */
std::optional<std::int32_t> to_int_or_null(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    if (index >= text.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
        if (value > 2'147'483'648LL) return std::nullopt;
    }
    if (negative) value = -value;
    if (value < -2'147'483'648LL || value > 2'147'483'647LL) return std::nullopt;
    return static_cast<std::int32_t>(value);
}

/** The Kotlin `e.message ?: fallback` mapping (C++ exceptions always carry a message; an empty
 *  one is the closest thing to the null case). */
std::string message_or(const std::exception& error, std::string_view fallback) {
    const std::string message = error.what();
    return message.empty() ? std::string(fallback) : message;
}

/** java.util.regex.Pattern.quote semantics on the std::regex ECMAScript dialect. */
std::string quote_regex(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (const char c : text) {
        switch (c) {
            case '^':
            case '$':
            case '\\':
            case '.':
            case '*':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
                out.push_back('\\');
                break;
            default:
                break;
        }
        out.push_back(c);
    }
    return out;
}

/** The SFTP channel scope of `withSftp`: open+connect on construction, disconnect on exit.
 *  Kotlin's connect sits outside the try/finally, so a failed connect leaves no disconnect. */
class SftpChannel {
public:
    SftpChannel(SshTransport& transport, std::int32_t timeout_ms) : transport_(transport) {
        transport_.open_sftp();
        transport_.connect_sftp(timeout_ms);
    }
    SftpChannel(const SftpChannel&) = delete;
    SftpChannel& operator=(const SftpChannel&) = delete;
    ~SftpChannel() {
        try {
            transport_.disconnect_sftp();
        } catch (...) {
            // The transport contract keeps disconnect non-throwing; a destructor must not escape.
        }
    }

private:
    SshTransport& transport_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// failedFileRead
// ---------------------------------------------------------------------------------------------

shell_tools::ShellFileReadResult failed_file_read(std::int64_t offset, std::int64_t limit,
                                                  std::string error) {
    shell_tools::ShellFileReadResult result;
    result.content = "";
    result.lines = 0;
    result.total_lines = 0;
    result.total_bytes = 0;
    result.returned_bytes = 0;
    result.offset = std::max<std::int64_t>(offset, 0);
    result.limit = (limit >= 1 && limit <= kFileReadMaxBytes) ? limit : kFileReadMaxBytes;
    result.truncated = false;
    result.error = std::move(error);
    return result;
}

// ---------------------------------------------------------------------------------------------
// SshClient — connection and TOFU
// ---------------------------------------------------------------------------------------------

class SshClient::TofuPolicy final : public HostKeyPolicy {
public:
    explicit TofuPolicy(SshClient& owner) : owner_(owner) {}

    int check(const std::string& host, const std::vector<std::uint8_t>& key) override {
        return owner_.check_host_key(host, key);
    }

private:
    SshClient& owner_;
};

SshClient::SshClient(SshTransport& transport, std::string host, std::int32_t port,
                     std::string user, std::string password, std::int32_t timeout_ms,
                     std::string pinned_host_key, bool allow_unknown_host_key)
    : transport_(transport),
      host_(std::move(host)),
      port_(port),
      user_(std::move(user)),
      password_(std::move(password)),
      timeout_ms_(timeout_ms),
      pinned_host_key_(std::move(pinned_host_key)),
      allow_unknown_host_key_(allow_unknown_host_key) {}

int SshClient::check_host_key(const std::string& /*host*/,
                              const std::vector<std::uint8_t>& key) {
    const std::string incoming = crypto::base64_encode(key, /*url_safe=*/false,
                                                       /*padding=*/true);
    captured_host_key_ = incoming;
    if (!json::is_blank(pinned_host_key_) && pinned_host_key_ == incoming) {
        return HostKeyPolicy::Ok;
    }
    if (!json::is_blank(pinned_host_key_)) return HostKeyPolicy::Changed;
    return HostKeyPolicy::NotIncluded;
}

void SshClient::ensure_session() {
    if (session_established_ && transport_.is_connected()) return;
    transport_.open_session(user_, host_, port_);
    transport_.set_password(password_);
    transport_.set_config("PreferredAuthentications", "password");
    TofuPolicy policy(*this);
    transport_.set_config(
        "StrictHostKeyChecking",
        (allow_unknown_host_key_ && json::is_blank(pinned_host_key_)) ? "no" : "yes");
    transport_.connect(timeout_ms_, policy);
    session_established_ = true;
}

void SshClient::close() {
    if (!session_established_) return;
    transport_.disconnect_session();
    session_established_ = false;
}

std::string SshClient::fingerprint_sha256(const std::string& base64_key) {
    std::string stripped;
    stripped.reserve(base64_key.size());
    for (const char c : base64_key) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') stripped.push_back(c);
    }
    const crypto::Base64Decode decoded = crypto::base64_decode(stripped, /*url_safe=*/false);
    if (!decoded.bytes) return "";
    const std::string_view bytes(reinterpret_cast<const char*>(decoded.bytes->data()),
                                 decoded.bytes->size());
    const std::array<std::uint8_t, 32> digest = sha256(bytes);
    return "SHA256:" + crypto::base64_encode(digest, /*url_safe=*/false, /*padding=*/false);
}

std::string SshClient::escape_bash(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('\'');
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// ---------------------------------------------------------------------------------------------
// SshClient — exec channel
// ---------------------------------------------------------------------------------------------

SshCommandResult SshClient::execute_command(const std::string& command,
                                            const std::string& workdir,
                                            std::int32_t exec_timeout_ms) {
    ensure_session();
    const std::string cmd = !json::is_blank(workdir)
                                ? "cd " + escape_bash(workdir) + " && " + command
                                : command;
    transport_.open_exec();
    transport_.set_command(cmd);
    bool disconnected = false;
    const auto disconnect_once = [&]() {
        if (!disconnected) {
            disconnected = true;
            transport_.disconnect_exec();
        }
    };
    try {
        transport_.connect_exec(timeout_ms_);
        const std::int64_t deadline =
            transport_.clock_millis() + std::max<std::int64_t>(exec_timeout_ms, 1'000);
        bool timed_out = false;
        while (!transport_.exec_is_closed()) {
            if (transport_.clock_millis() >= deadline) {
                timed_out = true;
                break;
            }
            transport_.sleep_millis(100);
        }
        const std::int32_t exit_code =
            transport_.exec_is_closed() ? transport_.exec_exit_status() : -1;
        if (timed_out) {
            throw std::runtime_error("Command timed out after " +
                                     std::to_string(exec_timeout_ms) + "ms");
        }
        SshCommandResult result;
        result.stdout_text = decode_utf8_replace(transport_.exec_stdout());
        result.stderr_text = decode_utf8_replace(transport_.exec_stderr());
        result.exit_code = exit_code;
        disconnect_once();
        return result;
    } catch (...) {
        disconnect_once();
        throw;
    }
}

// ---------------------------------------------------------------------------------------------
// SshClient — SFTP file operations
// ---------------------------------------------------------------------------------------------

namespace {

/** One fileRead readSlice: skip `start` bytes, then take up to `length` (a fresh stream). */
std::string read_slice(SshTransport& transport, const std::string& path, std::int64_t start,
                       std::int64_t length) {
    if (length == 0) return "";
    const std::string bytes = transport.get(path);
    const std::size_t begin = start <= 0
                                  ? 0
                                  : std::min<std::size_t>(static_cast<std::size_t>(start),
                                                          bytes.size());
    const std::size_t count =
        std::min<std::size_t>(static_cast<std::size_t>(length), bytes.size() - begin);
    return bytes.substr(begin, count);
}

}  // namespace

shell_tools::ShellFileReadResult SshClient::file_read(const std::string& path,
                                                      std::int64_t offset,
                                                      std::int64_t limit) {
    ensure_session();
    SftpChannel channel(transport_, timeout_ms_);
    try {
        const std::int64_t total_bytes = transport_.stat(path).size;
        const std::int64_t normalized_offset = std::max<std::int64_t>(offset, 0);
        const std::int64_t start = std::min(normalized_offset, total_bytes);
        const std::int64_t effective_limit =
            (limit >= 1 && limit <= kFileReadMaxBytes) ? limit : kFileReadMaxBytes;
        const std::string content_bytes =
            read_slice(transport_, path, start,
                       std::min<std::int64_t>(effective_limit, total_bytes - start));
        const std::string content = decode_utf8_replace(content_bytes);
        std::optional<std::string> complete_content;
        if (total_bytes <= kFileReadMaxBytes) {
            if (start == 0 &&
                static_cast<std::int64_t>(content_bytes.size()) == total_bytes) {
                complete_content = content;
            } else {
                complete_content =
                    decode_utf8_replace(read_slice(transport_, path, 0, total_bytes));
            }
        }
        shell_tools::ShellFileReadResult result;
        result.content = content;
        result.lines = shell_tools::shell_file_line_count(content);
        result.total_lines = complete_content
                                 ? shell_tools::shell_file_line_count(*complete_content)
                                 : 0;
        result.total_bytes = total_bytes;
        result.returned_bytes = static_cast<std::int64_t>(content.size());
        result.offset = start;
        result.limit = effective_limit;
        result.truncated = start + static_cast<std::int64_t>(content_bytes.size()) < total_bytes;
        return result;
    } catch (const std::exception& error) {
        throw std::runtime_error("SFTP read failed: " + std::string(error.what()));
    }
}

void SshClient::ensure_parent_dirs(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path) {
        if (c == '/') {
            if (!json::is_blank(part)) parts.push_back(std::move(part));
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!json::is_blank(part)) parts.push_back(std::move(part));

    std::string current = path.rfind('/', 0) == 0 ? "" : ".";
    for (const std::string& segment : parts) {
        current += "/" + segment;
        if (json::is_blank(current) || current == ".") continue;
        try {
            transport_.stat(current);
        } catch (...) {
            try {
                transport_.mkdir(current);
            } catch (...) {
                // Best effort, exactly like the Kotlin helper.
            }
        }
    }
}

std::optional<std::string> SshClient::file_write(const std::string& path,
                                                 const std::string& content) {
    ensure_session();
    SftpChannel channel(transport_, timeout_ms_);
    try {
        const std::string parent = substring_before_last(path, '/');
        if (!json::is_blank(parent)) ensure_parent_dirs(parent);
        transport_.put(path, content);
        return std::nullopt;
    } catch (const std::exception& error) {
        return "SFTP write failed: " + std::string(error.what());
    }
}

namespace {

/** readEditableFile: the Kotlin chunked loop stops once it has read past the 16 MiB cap. */
std::string read_editable_file(SshTransport& transport, const std::string& path) {
    const std::string bytes = transport.get(path);
    constexpr std::size_t kMax = static_cast<std::size_t>(16'777'216) + 8192;
    if (bytes.size() <= kMax) return bytes;
    return bytes.substr(0, kMax);
}

}  // namespace

shell_file::ShellFileEditResult SshClient::file_edit(const std::string& path,
                                                     const std::string& old_string,
                                                     const std::string& new_string,
                                                     bool replace_all) {
    ensure_session();
    SftpChannel channel(transport_, timeout_ms_);
    try {
        const SshTransport::SftpAttrs attrs = transport_.stat(path);
        if (attrs.is_dir) {
            return shell_file::ShellFileEditResult{0, "", std::string("path is not a regular file")};
        }
        if (attrs.size > kFileEditMaxBytes) {
            return shell_file::ShellFileEditResult{0, "", std::string("file exceeds 16MB edit limit")};
        }
        const std::string original_bytes = read_editable_file(transport_, path);
        if (static_cast<std::int64_t>(original_bytes.size()) > kFileEditMaxBytes) {
            return shell_file::ShellFileEditResult{0, "", std::string("file exceeds 16MB edit limit")};
        }
        const std::u16string content = utf16::from_utf8(original_bytes);
        auto edited = shell_file::edit_shell_file_content(
            content, utf16::from_utf8(old_string), utf16::from_utf8(new_string), replace_all);
        if (!edited.first) return edited.second;
        const std::string edited_bytes = utf16::to_utf8(*edited.first);
        if (static_cast<std::int64_t>(edited_bytes.size()) > kFileEditMaxBytes) {
            return shell_file::ShellFileEditResult{0, "",
                                                   std::string("edited file exceeds 16MB limit")};
        }
        if (read_editable_file(transport_, path) != original_bytes) {
            return shell_file::ShellFileEditResult{
                0, "", std::string("file changed concurrently before edit could be applied")};
        }
        transport_.put(path, edited_bytes);
        shell_file::ShellFileEditResult result = edited.second;
        result.sha256 = shell_file::shell_file_sha256(edited_bytes);
        return result;
    } catch (const std::exception& error) {
        return shell_file::ShellFileEditResult{0, "", "SFTP edit failed: " +
                                                          std::string(error.what())};
    }
}

void SshClient::sftp_list_recursive(const std::string& dir, std::vector<std::string>& result,
                                    std::int32_t remaining) {
    try {
        const std::vector<SshTransport::LsEntry> entries = transport_.ls(dir);
        for (const SshTransport::LsEntry& entry : entries) {
            const std::string& name = entry.filename;
            if (name == "." || name == "..") continue;
            const std::string full_path = dir + "/" + name;
            if (entry.is_dir) {
                if (remaining < 0 || remaining > 1) {
                    sftp_list_recursive(full_path, result, remaining < 0 ? -1 : remaining - 1);
                }
            } else {
                result.push_back(full_path);
            }
        }
    } catch (...) {
        // Permission denied or directory doesn't exist — skip.
    }
}

std::vector<std::string> SshClient::glob_match(const std::vector<std::string>& files,
                                               const std::string& base_path,
                                               const std::string& pattern) {
    const std::string adjusted =
        pattern.find('/') != std::string::npos ? pattern : "**/" + pattern;
    const std::string full_pattern = base_path + "/" + adjusted;
    std::vector<std::string> matches;
    for (const std::string& file : files) {
        if (glob::matches(full_pattern, file)) matches.push_back(file);
    }
    return matches;
}

std::pair<std::vector<std::string>, bool> SshClient::file_glob(
    const std::string& pattern, const std::string& base_path,
    const std::optional<std::int32_t>& depth) {
    ensure_session();
    SftpChannel channel(transport_, timeout_ms_);
    std::string base = base_path;
    if (json::is_blank(base)) {
        try {
            base = transport_.pwd();
        } catch (...) {
            base = "/";
        }
    }
    base = trim_end_char(base, '/');
    std::vector<std::string> all_files;
    const std::int32_t remaining = (!depth.has_value() || *depth <= 0) ? -1 : *depth;
    sftp_list_recursive(base, all_files, remaining);
    std::vector<std::string> matches = glob_match(all_files, base, pattern);
    const bool truncated =
        static_cast<std::int32_t>(matches.size()) >= kFileGlobMaxMatches;
    if (matches.size() > static_cast<std::size_t>(kFileGlobMaxMatches)) {
        matches.resize(kFileGlobMaxMatches);
    }
    return {std::move(matches), truncated};
}

// ---------------------------------------------------------------------------------------------
// SshClient — grep
// ---------------------------------------------------------------------------------------------

std::optional<GrepMatch> SshClient::parse_grep_line(const std::string& line) {
    const std::size_t first_colon = line.find(':');
    if (first_colon == std::string::npos) return std::nullopt;
    const std::string path = line.substr(0, first_colon);
    const std::string after_path = line.substr(first_colon + 1);
    const std::size_t second_colon = after_path.find(':');
    if (second_colon == std::string::npos) return std::nullopt;
    const std::string line_number_text = after_path.substr(0, second_colon);
    const std::string content =
        json::take_utf16_units(after_path.substr(second_colon + 1), kFileGrepMaxContentChars);
    const std::optional<std::int32_t> line_number = to_int_or_null(line_number_text);
    if (!line_number) return std::nullopt;
    return GrepMatch{path, *line_number, content};
}

GrepOutcome SshClient::fallback_grep(const std::string& regex, const std::string& base_path,
                                     const std::string& file_glob) {
    try {
        const std::string glob_pattern = json::is_blank(file_glob) ? "*" : file_glob;
        const std::vector<std::string> files =
            this->file_glob(glob_pattern, base_path, std::nullopt).first;
        std::regex pattern;
        try {
            pattern = std::regex(regex, std::regex::ECMAScript);
        } catch (const std::regex_error&) {
            pattern = std::regex(quote_regex(regex), std::regex::ECMAScript);
        }
        std::vector<GrepMatch> matches;
        for (const std::string& file : files) {
            try {
                const std::string content =
                    file_read(file, 0, kFileGrepMaxFileBytes).content;
                // Skip binary files (NUL-byte heuristic), as grep -I would.
                if (content.find('\0') != std::string::npos) continue;
                const std::vector<std::string> lines = lines_of(content);
                for (std::size_t index = 0; index < lines.size(); ++index) {
                    if (std::regex_search(lines[index], pattern)) {
                        matches.push_back(GrepMatch{
                            file, static_cast<std::int32_t>(index + 1),
                            json::take_utf16_units(lines[index], kFileGrepMaxContentChars)});
                        if (static_cast<std::int32_t>(matches.size()) >=
                            kFileGrepMaxMatches) {
                            return GrepOutcome{true, std::move(matches), true, ""};
                        }
                    }
                }
            } catch (...) {
                // Skip unreadable files.
            }
        }
        return GrepOutcome{true, std::move(matches), false, ""};
    } catch (const std::exception& error) {
        return GrepOutcome{false, {}, false, error.what()};
    }
}

GrepOutcome SshClient::file_grep(const std::string& pattern, const std::string& base_path,
                                 const std::string& file_glob) {
    const std::string base = json::is_blank(base_path) ? "." : base_path;
    try {
        std::string command = "grep -rnI ";
        if (!json::is_blank(file_glob)) {
            command += "--include=" + escape_bash(file_glob) + " ";
        }
        command += "-- " + escape_bash(pattern) + " " + escape_bash(base);
        const SshCommandResult result = execute_command(command, "", timeout_ms_);
        if (result.exit_code == 0 || result.exit_code == 1) {
            std::vector<GrepMatch> matches;
            for (const std::string& line : lines_of(result.stdout_text)) {
                if (json::is_blank(line)) continue;
                std::optional<GrepMatch> match = parse_grep_line(line);
                if (match) matches.push_back(std::move(*match));
            }
            const bool truncated =
                static_cast<std::int32_t>(matches.size()) >= kFileGrepMaxMatches;
            if (matches.size() > static_cast<std::size_t>(kFileGrepMaxMatches)) {
                matches.resize(kFileGrepMaxMatches);
            }
            return GrepOutcome{true, std::move(matches), truncated, ""};
        }
        return fallback_grep(pattern, base, file_glob);
    } catch (...) {
        return fallback_grep(pattern, base, file_glob);
    }
}

// ---------------------------------------------------------------------------------------------
// ConchBackend
// ---------------------------------------------------------------------------------------------

namespace {

/** `handle.errorBody?.take(240)?.ifBlank { "empty response" } ?: "empty response"`. */
std::string http_error_detail(const std::optional<std::string>& body) {
    if (!body) return "empty response";
    const std::string taken = json::take_utf16_units(*body, 240);
    if (json::is_blank(taken)) return "empty response";
    return taken;
}

}  // namespace

ConchBackend::ConchBackend(ShellDeviceConfig device, client::ConchTransport& transport,
                           HttpStreamTransport& stream_transport)
    : device_(std::move(device)),
      url_(trim_end_char(device_->server_url, '/')),
      api_key_(device_->api_key),
      device_name_(device_->name),
      client_(url_, api_key_, device_->conch_public_key, transport),
      stream_transport_(stream_transport) {}

std::string ConchBackend::execute_command(const std::string& command,
                                          const std::string& workdir,
                                          std::int32_t timeout_ms) {
    return execute_command_internal(command, workdir, timeout_ms,
                                    [](const std::string&) {});
}

void ConchBackend::execute_command_events(const std::string& command,
                                          const std::string& workdir,
                                          std::int32_t timeout_ms,
                                          const BackendEventSink& sink) {
    const std::string result = execute_command_internal(
        command, workdir, timeout_ms, [&](const std::string& delta) {
            sink(BackendEvent{BackendEvent::Kind::OutputDelta, delta, false});
        });
    sink(BackendEvent{BackendEvent::Kind::Completed, result, false});
}

std::string ConchBackend::execute_command_internal(
    const std::string& command, const std::string& workdir, std::int32_t timeout_ms,
    const std::function<void(const std::string&)>& on_output) {
    if (json::is_blank(url_)) {
        return shell_tools::json_error(
            "execute_shell_command",
            "Server \"" + device_name_ + "\" has no URL configured.", std::nullopt,
            std::nullopt);
    }
    if (!client_.fetch_public_key() && !json::is_blank(api_key_)) {
        return shell_tools::json_error(
            "execute_shell_command",
            client_.last_error().value_or("Conch public-key exchange failed for " + url_),
            device_name_, std::nullopt);
    }
    const client::ShellClient::PreparedRequest prepared =
        client_.prepare_request(command, timeout_ms, workdir);
    std::unique_ptr<HttpStreamHandle> handle;
    try {
        handle = stream_transport_.stream_post(StreamCall{
            prepared.server_url + "/execute", prepared.body, prepared.headers, kLineLimit,
            kControlLimit});
    } catch (const client::TransportFailure& failure) {
        return shell_tools::json_error(
            "execute_shell_command",
            client::describe_conch_request_failure(prepared.server_url, "/execute request",
                                                   failure),
            device_name_, command);
    } catch (const std::exception& failure) {
        // The Kotlin `else` arm: message (or simple class name) under the generic wrap.
        return shell_tools::json_error(
            "execute_shell_command",
            "Conch /execute request to " + prepared.server_url + " failed: " +
                message_or(failure, "Exception"),
            device_name_, command);
    }
    if (handle->code() < 200 || handle->code() > 299) {
        const std::string detail = http_error_detail(handle->error_body());
        const std::int32_t code = handle->code();
        handle->close();
        return shell_tools::json_error(
            "execute_shell_command",
            "Conch at " + prepared.server_url + " returned HTTP " + std::to_string(code) +
                ": " + detail,
            device_name_, command);
    }
    try {
        const conch::SseResult result = conch::parse_conch_sse_lines(
            prepared.is_encrypted, [&]() { return handle->read_line(); },
            [&](const std::string& data) { return client_.decrypt_sse_data(data); },
            on_output);
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("type", json_string("execute_shell_command"));
        members.emplace_back("server", json_string(device_name_));
        members.emplace_back("command", json_string(command));
        if (result.error_message.has_value()) {
            members.emplace_back("error", json_string("execution_error"));
            members.emplace_back("message", json_string(*result.error_message));
            if (result.timed_out) members.emplace_back("timed_out", json_bool(true));
        } else {
            members.emplace_back("exit_code",
                                 json_literal(std::to_string(result.exit_code.value_or(-1))));
        }
        if (result.warning_message.has_value()) {
            members.emplace_back("warning", json_string(*result.warning_message));
        }
        members.emplace_back("output", json_string(result.output));
        const std::string text = json::to_kotlin_string(json_object(std::move(members)));
        handle->close();
        return text;
    } catch (const std::exception& error) {
        handle->close();
        return shell_tools::json_error("execute_shell_command",
                                       message_or(error, "Unknown error"), device_name_,
                                       command);
    }
}

shell_tools::ShellFileReadResult ConchBackend::file_read(const std::string& path,
                                                         std::int64_t offset,
                                                         std::int64_t limit) {
    return client_.file_read(path, offset, limit);
}

std::optional<std::string> ConchBackend::file_write(const std::string& path,
                                                    const std::string& content) {
    std::optional<std::string> error = client_.file_write(path, content);
    if (!error) return std::nullopt;
    return shell_tools::json_error("file_write", *error, device_name_, std::nullopt);
}

shell_file::ShellFileEditResult ConchBackend::file_edit(const std::string& path,
                                                        const std::string& old_string,
                                                        const std::string& new_string,
                                                        bool replace_all) {
    return client_.file_edit(path, old_string, new_string, replace_all);
}

GlobOutcome ConchBackend::file_glob(const std::string& pattern, const std::string& base_path,
                                    const std::optional<std::int32_t>& depth) {
    client::ShellClient::GlobOutcome result = client_.file_glob(pattern, base_path, depth);
    GlobOutcome outcome;
    outcome.ok = result.ok;
    outcome.files = std::move(result.files);
    outcome.truncated = result.truncated;
    outcome.error = std::move(result.error);
    return outcome;
}

GrepOutcome ConchBackend::file_grep(const std::string& pattern, const std::string& base_path,
                                    const std::string& file_glob) {
    client::ShellClient::GrepOutcome result = client_.file_grep(pattern, base_path, file_glob);
    GrepOutcome outcome;
    outcome.ok = result.ok;
    outcome.truncated = result.truncated;
    outcome.error = std::move(result.error);
    for (auto& match : result.matches) {
        outcome.matches.push_back(GrepMatch{std::move(match.path), match.line,
                                            std::move(match.content)});
    }
    return outcome;
}

std::string ConchBackend::start_job(const std::string& command, std::int32_t timeout_ms,
                                    const std::string& workdir) {
    return client_.start_job(command, timeout_ms, workdir);
}

std::string ConchBackend::list_jobs() { return client_.list_jobs(); }

std::string ConchBackend::get_job(const std::string& job_id) {
    return client_.get_job(job_id);
}

std::string ConchBackend::stop_job(const std::string& job_id,
                                   const std::optional<std::int64_t>& call_timeout_millis) {
    return client_.stop_job(job_id, call_timeout_millis);
}

std::string ConchBackend::acknowledge_job(const std::string& job_id) {
    return client_.acknowledge_job(job_id);
}

client::ShellClient::FileImageResult ConchBackend::view_image(const std::string& path) {
    return client_.file_image(path);
}

// ---------------------------------------------------------------------------------------------
// SshBackend
// ---------------------------------------------------------------------------------------------

SshBackend::SshBackend(ShellDeviceConfig device, SshTransport& transport)
    : device_(std::move(device)),
      host_(device_->ssh_host),
      device_name_(device_->name),
      client_(transport, host_, device_->ssh_port, device_->ssh_user, device_->ssh_password,
              /*timeout_ms=*/30'000, device_->ssh_host_key,
              /*allow_unknown_host_key=*/json::is_blank(device_->ssh_host_key)) {}

std::string SshBackend::execute_command(const std::string& command,
                                        const std::string& workdir,
                                        std::int32_t timeout_ms) {
    if (json::is_blank(host_)) {
        return shell_tools::json_error(
            "execute_shell_command",
            "SSH device \"" + device_name_ + "\" has no host configured.", std::nullopt,
            std::nullopt);
    }
    try {
        const SshCommandResult result = client_.execute_command(command, workdir, timeout_ms);
        std::string output = result.stdout_text;
        if (!json::is_blank(result.stderr_text)) {
            output += "\n" + result.stderr_text;
        }
        output = trim_end_whitespace(output);
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("type", json_string("execute_shell_command"));
        members.emplace_back("server", json_string(device_name_));
        members.emplace_back("command", json_string(command));
        members.emplace_back("exit_code", json_literal(std::to_string(result.exit_code)));
        members.emplace_back("output", json_string(output));
        return json::to_kotlin_string(json_object(std::move(members)));
    } catch (const std::exception& error) {
        return shell_tools::json_error("execute_shell_command",
                                       message_or(error, "Unknown error"), device_name_,
                                       command);
    }
}

shell_tools::ShellFileReadResult SshBackend::file_read(const std::string& path,
                                                       std::int64_t offset,
                                                       std::int64_t limit) {
    try {
        return client_.file_read(path, offset, limit);
    } catch (const std::exception& error) {
        return failed_file_read(offset, limit,
                                "SFTP read failed: " + std::string(error.what()));
    }
}

std::optional<std::string> SshBackend::file_write(const std::string& path,
                                                  const std::string& content) {
    std::optional<std::string> error = client_.file_write(path, content);
    if (!error) return std::nullopt;
    return shell_tools::json_error("file_write", *error, device_name_, std::nullopt);
}

shell_file::ShellFileEditResult SshBackend::file_edit(const std::string& path,
                                                      const std::string& old_string,
                                                      const std::string& new_string,
                                                      bool replace_all) {
    return client_.file_edit(path, old_string, new_string, replace_all);
}

GlobOutcome SshBackend::file_glob(const std::string& pattern, const std::string& base_path,
                                  const std::optional<std::int32_t>& depth) {
    auto result = client_.file_glob(pattern, base_path, depth);
    GlobOutcome outcome;
    outcome.ok = true;
    outcome.files = std::move(result.first);
    outcome.truncated = result.second;
    return outcome;
}

GrepOutcome SshBackend::file_grep(const std::string& pattern, const std::string& base_path,
                                  const std::string& file_glob) {
    return client_.file_grep(pattern, base_path, file_glob);
}

// ---------------------------------------------------------------------------------------------
// SandboxBackend
// ---------------------------------------------------------------------------------------------

SandboxBackend::SandboxBackend(SandboxManager* manager)
    : device_(std::nullopt), manager_([&]() -> SandboxManager& {
          if (manager == nullptr) throw std::runtime_error("Sandbox not available");
          return *manager;
      }()) {}

std::string SandboxBackend::execute_command(const std::string& command,
                                            const std::string& workdir,
                                            std::int32_t timeout_ms) {
    if (!manager_.is_available()) {
        return shell_tools::json_error("execute_shell_command",
                                       "Local Sandbox is not installed.", std::nullopt,
                                       std::nullopt);
    }
    try {
        const SandboxManager::SandboxResult result =
            manager_.execute_command(command, workdir, timeout_ms);
        std::string output = result.stdout_text;
        if (!json::is_blank(result.stderr_text)) {
            output += "\n" + result.stderr_text;
        }
        output = trim_end_whitespace(output);
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("type", json_string("execute_shell_command"));
        members.emplace_back("server", json_string("Local Sandbox"));
        members.emplace_back("command", json_string(command));
        members.emplace_back("exit_code", json_literal(std::to_string(result.exit_code)));
        if (result.warning.has_value()) {
            members.emplace_back("warning", json_string(*result.warning));
        }
        members.emplace_back("output", json_string(output));
        return json::to_kotlin_string(json_object(std::move(members)));
    } catch (const std::exception& error) {
        return shell_tools::json_error("execute_shell_command",
                                       message_or(error, "Unknown error"), "Local Sandbox",
                                       command);
    }
}

shell_tools::ShellFileReadResult SandboxBackend::file_read(const std::string& path,
                                                           std::int64_t offset,
                                                           std::int64_t limit) {
    try {
        return manager_.file_read(path, offset, limit);
    } catch (const std::exception& error) {
        return failed_file_read(offset, limit, message_or(error, "Read failed"));
    }
}

std::optional<std::string> SandboxBackend::file_write(const std::string& path,
                                                      const std::string& content) {
    std::optional<std::string> error = manager_.file_write(path, content);
    if (!error) return std::nullopt;
    return shell_tools::json_error("file_write", *error, "Local Sandbox", std::nullopt);
}

shell_file::ShellFileEditResult SandboxBackend::file_edit(const std::string& path,
                                                          const std::string& old_string,
                                                          const std::string& new_string,
                                                          bool replace_all) {
    return manager_.file_edit(path, old_string, new_string, replace_all);
}

GlobOutcome SandboxBackend::file_glob(const std::string& pattern, const std::string& base_path,
                                      const std::optional<std::int32_t>& depth) {
    try {
        auto result = manager_.file_glob(pattern, base_path, depth);
        GlobOutcome outcome;
        outcome.ok = true;
        outcome.files = std::move(result.first);
        outcome.truncated = result.second;
        return outcome;
    } catch (const std::exception& error) {
        return GlobOutcome{false, {}, false, error.what()};
    }
}

GrepOutcome SandboxBackend::file_grep(const std::string& pattern, const std::string& base_path,
                                      const std::string& file_glob) {
    GrepOutcome result = manager_.file_grep(pattern, base_path, file_glob);
    if (!result.ok) return result;
    GrepOutcome outcome;
    outcome.ok = true;
    outcome.truncated = result.truncated;
    for (auto& match : result.matches) {
        outcome.matches.push_back(GrepMatch{std::move(match.path), match.line,
                                            std::move(match.content)});
    }
    return outcome;
}

}  // namespace agora::core::shell_backends
