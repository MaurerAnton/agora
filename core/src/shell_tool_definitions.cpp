// ShellToolDefinitions.kt — transcribed from the app file (see
// core/tools/golden/shell-definitions for the verbatim Kotlin the traces are frozen from).
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "agora/core/openai_request.hpp"
#include "agora/core/shell_tools.hpp"

namespace agora::core::shell_tools {
namespace {

namespace openai = agora::core::openai;

constexpr std::int64_t kWaitJobOuterMarginMs = 5000;
constexpr std::int32_t kMinWaitJobMs = 1000;

openai::ToolProperty prop(std::string type, std::string description) {
    openai::ToolProperty property;
    property.type = std::move(type);
    property.description = std::move(description);
    return property;
}

openai::ToolDefinition tool(std::string name, std::string description,
                            std::vector<std::pair<std::string, openai::ToolProperty>> properties,
                            std::vector<std::string> required) {
    openai::ToolDefinition definition;
    definition.function.name = std::move(name);
    definition.function.description = std::move(description);
    definition.function.parameters.properties = std::move(properties);
    definition.function.parameters.required = std::move(required);
    return definition;
}

std::string join(const std::vector<std::string>& values, const std::string& separator) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += separator;
        out += values[i];
    }
    return out;
}

}  // namespace

std::int32_t max_wait_ms(const ShellDefinitionsConfig& config) {
    const std::int64_t remaining =
        static_cast<std::int64_t>(config.tool_timeout_ms) - kWaitJobOuterMarginMs;
    return static_cast<std::int32_t>(std::max<std::int64_t>(remaining, kMinWaitJobMs));
}

std::vector<openai::ToolDefinition> build_shell_tool_definitions(
    const ShellDefinitionsConfig& config) {
    std::vector<openai::ToolDefinition> tools;
    if (!config.shell_enabled) return tools;
    if (config.devices.empty() && !config.sandbox_enabled) return tools;

    // Device names are quoted for the model-facing descriptions.
    std::vector<std::string> all_device_names;
    if (config.sandbox_enabled) all_device_names.push_back("Local Sandbox");
    for (const ShellDeviceConfig& device : config.devices) {
        all_device_names.push_back("\"" + device.name + "\"");
    }
    const std::string device_names_str = join(all_device_names, ", ");
    const bool single_server = all_device_names.size() == 1;

    const std::string server_prop_desc =
        single_server
            ? "The shell server name (optional, defaults to the only available server: " +
                  all_device_names[0] + ")."
            : "The shell server name. Use list_shells to see available servers: " +
                  device_names_str + ".";

    std::vector<std::string> shell_required = single_server
                                                  ? std::vector<std::string>{"command", "timeout_ms"}
                                                  : std::vector<std::string>{"command", "server",
                                                                            "timeout_ms"};

    std::vector<std::string> conch_device_names;
    for (const ShellDeviceConfig& device : config.devices) {
        if (device.type == "ssh") continue;
        const bool blank = device.name.find_first_not_of(" \t\n\r\f\v") == std::string::npos;
        conch_device_names.push_back(blank ? std::string("Untitled") : device.name);
    }
    const bool single_conch = conch_device_names.size() == 1;

    tools.push_back(tool("list_shells",
                         "List configured shell servers including the local sandbox (if enabled).",
                         {}, {}));
    tools.push_back(tool(
        "execute_shell_command",
        "Execute a shell command with a 64KB UTF-8 command limit, a 32KB UTF-8 workdir limit, and "
        "at most 1MB of retained foreground output. Set background=true for a durable Conch job "
        "that survives client disconnects.",
        {
            {"command", prop("string", "The shell command to execute (64KB UTF-8 maximum).")},
            {"server", prop("string", server_prop_desc)},
            {"timeout_ms",
             prop("integer",
                  "Required. Foreground wait budget in milliseconds (hard ceiling 295000ms inside "
                  "the tool call). On Conch, a command still running at that point continues as "
                  "the same durable job and returns its job_id; it is never killed or restarted. "
                  "With background=true this instead bounds the durable job's runtime (up to Conch "
                  "policy).")},
            {"workdir", prop("string", "Working directory (optional, 32KB UTF-8 maximum).")},
            {"background",
             prop("boolean",
                  "Start a durable background job on Conch and return its job_id immediately "
                  "(optional, default false).")},
        },
        shell_required));

    if (!conch_device_names.empty()) {
        const std::string job_server_description =
            single_conch
                ? "Conch server name (optional; defaults to " + conch_device_names[0] + ")."
                : "Conch server name. Available: " + join(conch_device_names, ", ") + ".";
        const std::vector<std::string> job_required =
            single_conch ? std::vector<std::string>{} : std::vector<std::string>{"server"};

        auto with_job_required = [&](std::vector<std::string> base) {
            base.insert(base.end(), job_required.begin(), job_required.end());
            return base;
        };

        tools.push_back(tool("list_shell_jobs",
                             "List durable background shell jobs on a Conch server.",
                             {{"server", prop("string", job_server_description)}}, job_required));
        tools.push_back(tool("get_shell_job",
                             "Get status and bounded output for a durable Conch shell job. Prefer "
                             "wait_for_job for blocking use.",
                             {{"job_id", prop("string", "The Conch job id.")},
                              {"server", prop("string", job_server_description)}},
                             with_job_required({"job_id"})));
        tools.push_back(tool(
            "wait_for_job",
            "Block until a durable Conch shell job finishes or timeout_ms elapses while updating "
            "one shell-style output block with bounded full cumulative snapshots. A timed-out "
            "result includes the current full retained output and leaves the job running; call "
            "wait_for_job again to keep waiting.",
            {{"job_id", prop("string", "The Conch job id.")},
             {"timeout_ms",
              prop("integer",
                   "Required. Maximum time to block in milliseconds before returning (whether or "
                   "not the job finished). The hard ceiling for a single call is " +
                       std::to_string(max_wait_ms(config)) +
                       "ms; larger values are clamped to it and the result says so. To wait longer, "
                       "call wait_for_job again.")},
             {"server", prop("string", job_server_description)}},
            with_job_required({"job_id", "timeout_ms"})));
        tools.push_back(tool("stop_shell_job",
                             "Stop a running durable Conch shell job and its process tree.",
                             {{"job_id", prop("string", "The Conch job id.")},
                              {"server", prop("string", job_server_description)}},
                             with_job_required({"job_id"})));
    }

    const openai::ToolProperty file_server_property =
        single_server
            ? prop("string", "The shell server name (optional, defaults to the only available "
                             "server).")
            : prop("string", "The shell server name. Available: " + device_names_str + ".");
    const std::vector<std::string> file_required =
        single_server ? std::vector<std::string>{} : std::vector<std::string>{"server"};
    auto with_file_required = [&](std::vector<std::string> base) {
        base.insert(base.end(), file_required.begin(), file_required.end());
        return base;
    };

    tools.push_back(tool(
        "file_read",
        "Read a bounded file slice from a shell server or local sandbox. Returns valid JSON with "
        "content, line and byte counts, offset, limit, and an explicit truncated flag.",
        {{"path", prop("string", "Absolute path to the file.")},
         {"server", file_server_property},
         {"offset", prop("integer", "Byte offset (optional).")},
         {"limit", prop("integer", "Max bytes to read (optional, default and maximum 1MB).")}},
        with_file_required({"path"})));
    tools.push_back(tool(
        "file_write",
        "Write up to 1MB of UTF-8 content to a file on a shell server or local sandbox.",
        {{"path", prop("string", "Absolute path to the file.")},
         {"content", prop("string", "Content to write (1MB UTF-8 maximum).")},
         {"server", file_server_property}},
        with_file_required({"path", "content"})));
    tools.push_back(tool(
        "file_edit",
        "Edit a file on a shell server or local sandbox by replacing old_string with new_string.",
        {{"path", prop("string", "Absolute path to the file.")},
         {"old_string",
          prop("string", "The exact text to find and replace. LF and CRLF line endings are treated "
                         "as equivalent.")},
         {"new_string",
          prop("string",
               "The replacement text. Line endings follow the matched text or target file.")},
         {"server", file_server_property},
         {"replace_all", prop("boolean", "Replace all occurrences (optional, default false).")}},
        with_file_required({"path", "old_string", "new_string"})));
    tools.push_back(tool(
        "file_glob",
        "List up to 1000 files on a shell server or local sandbox matching a glob pattern. Returns "
        "an explicit truncated flag.",
        {{"pattern",
          prop("string", "Glob pattern matched against file names (e.g. '*.go', '*.md').")},
         {"server", file_server_property},
         {"path", prop("string", "Base directory for the search (optional).")},
         {"depth",
          prop("integer",
               "Max directory levels to search below 'path': 1 = base directory only, higher "
               "values recurse deeper, 0 = unlimited recursion. Omit for the server default.")}},
        with_file_required({"pattern"})));
    tools.push_back(tool(
        "file_grep",
        "Search for up to 500 regex matches in files on a shell server or local sandbox. Returns "
        "an explicit truncated flag.",
        {{"pattern", prop("string", "Regular expression pattern to search for.")},
         {"server", file_server_property},
         {"path", prop("string", "File or directory to search in (optional).")},
         {"glob", prop("string", "Filter files by glob pattern (optional).")}},
        with_file_required({"pattern"})));

    if (!conch_device_names.empty()) {
        const std::string image_server_description =
            single_conch
                ? "Conch server name (optional; defaults to " + conch_device_names[0] + ")."
                : "Conch server name. Available: " + join(conch_device_names, ", ") + ".";
        const std::vector<std::string> image_required =
            single_conch ? std::vector<std::string>{"path"}
                         : std::vector<std::string>{"path", "server"};
        tools.push_back(tool(
            "view_image",
            "Load an image from a Conch device and return it as visual context.",
            {{"path", prop("string", "Absolute path to the image file.")},
             {"server", prop("string", image_server_description)}},
            image_required));
    }

    return tools;
}

}  // namespace agora::core::shell_tools