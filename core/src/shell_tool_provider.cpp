// tool/ShellToolProvider.kt — transcribed from the app file (see
// core/tools/golden/shell-tool-provider for the verbatim Kotlin the traces are frozen from).
//
// The provider is a pure dispatch layer over the already-pinned shell backends, durable-job
// executor and definitions builder. The platform seams are:
//   * BackendFactory     — where Kotlin writes `ConchBackend(device)` / `SshBackend(device)`;
//   * SandboxFactory     — sandbox/SandboxManagerFactory.kt (create() once, isAvailable());
//   * ToolImageStore     — tool/ToolImageStore.kt's Android image publication;
//   * DurableJobHooks    — System.currentTimeMillis / delay / isActive.
//
// The synchronous port keeps every Kotlin branch and JSON envelope; coroutine cancellation is
// surfaced as durable_jobs::JobCancelled and the 3s acknowledgement timeout stays a transport
// concern.
#include "agora/core/shell_tool_provider.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include "agora/core/json.hpp"
#include "agora/core/shell_tools.hpp"

namespace agora::core::shell_provider {
namespace {

namespace json = agora::core::json;
namespace backends = agora::core::shell_backends;
namespace client = agora::core::shell_client;
namespace durable = agora::core::durable_jobs;
namespace shell_tools = agora::core::shell_tools;

constexpr std::int32_t kForegroundMaxMs = 300'000;  // Constants.TOOL_EXECUTION_TIMEOUT_MS.toInt()
constexpr std::int32_t kBackgroundJobMaxMs = 86'400'000;  // BACKGROUND_JOB_MAX_MS
constexpr std::int32_t kMinTimeoutMs = 1'000;
constexpr std::size_t kCommandMaxBytes = 64 * 1024;   // SHELL_COMMAND_MAX_BYTES
constexpr std::size_t kWorkdirMaxBytes = 32 * 1024;   // SHELL_WORKDIR_MAX_BYTES
constexpr std::size_t kFileWriteMaxBytes = 1024 * 1024;  // SHELL_FILE_WRITE_MAX_BYTES

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// ---- JSON construction (kotlinx buildJsonObject key order) ---------------------------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_number(std::string digits) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(digits);
    return value;
}

json::Value json_bool(bool flag) { return json_number(flag ? "true" : "false"); }

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

std::optional<std::string> present(const std::string& text) {
    return std::optional<std::string>(text);
}

// ---- Kotlin string → number conversions ------------------------------------------------------

std::optional<std::int64_t> parse_long_or_null(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    if (index == text.size()) return std::nullopt;
    const std::uint64_t limit =
        negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (limit - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    if (negative) {
        if (value == 9223372036854775808ULL) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return -static_cast<std::int64_t>(value);
    }
    return static_cast<std::int64_t>(value);
}

std::optional<std::int32_t> parse_int_or_null(std::string_view text) {
    const auto value = parse_long_or_null(text);
    if (!value) return std::nullopt;
    if (*value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*value);
}

std::optional<std::string> shell_command_validation_error(const std::string& command,
                                                          const std::string& workdir) {
    if (command.size() > kCommandMaxBytes) return std::string("command exceeds 64KB limit");
    if (workdir.size() > kWorkdirMaxBytes) return std::string("workdir exceeds 32KB limit");
    return std::nullopt;
}

std::int32_t clamp_timeout(std::int32_t value, std::int32_t maximum) {
    return std::max(kMinTimeoutMs, std::min(value, maximum));
}

durable::DurableJobHooks default_hooks() {
    durable::DurableJobHooks hooks;
    hooks.now = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    hooks.delay = [](std::int64_t millis) {
        std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    };
    hooks.is_active = [] { return true; };
    return hooks;
}

/** Kotlin `finally { backend.close() }` for every provider branch. */
class BackendCloser {
public:
    explicit BackendCloser(backends::Backend* backend) : backend_(backend) {}
    ~BackendCloser() {
        if (backend_ != nullptr) backend_->close();
    }
    BackendCloser(const BackendCloser&) = delete;
    BackendCloser& operator=(const BackendCloser&) = delete;

private:
    backends::Backend* backend_;
};

/** ConchBackend as the durable-jobs transport (the executor itself is transport-agnostic). */
class ConchJobAdapter final : public durable::DurableJobBackend {
public:
    ConchJobAdapter(backends::ConchBackend& backend, std::string device_name)
        : backend_(backend), device_name_(std::move(device_name)) {}

    const std::string& device_name() const noexcept override { return device_name_; }

    std::string start_job(const std::string& command, const std::string& workdir,
                          std::int32_t timeout_ms) override {
        return backend_.start_job(command, timeout_ms, workdir);
    }

    std::string get_job(const std::string& job_id) override {
        return backend_.get_job(job_id);
    }

    std::string list_jobs() override { return backend_.list_jobs(); }

    std::string stop_job(const std::string& job_id,
                         std::optional<std::int64_t> call_timeout_ms) override {
        return backend_.stop_job(job_id, call_timeout_ms);
    }

    void close() override { backend_.close(); }

private:
    backends::ConchBackend& backend_;
    std::string device_name_;
};

/** The `makeBackend` factory durable_jobs uses for the public job entry points. */
class JobBackendFactory {
public:
    JobBackendFactory(BackendFactory& factory, const std::vector<ShellDeviceConfig>& devices)
        : factory_(factory), devices_(devices) {}

    durable::DurableJobBackend& make(const shell_tools::ShellDeviceConfig& device) {
        const ShellDeviceConfig* full = nullptr;
        for (const ShellDeviceConfig& candidate : devices_) {
            if (candidate.type == "ssh") continue;
            if (ascii_iequals(candidate.name, device.name)) {
                full = &candidate;
                break;
            }
        }
        if (full == nullptr) {
            throw std::runtime_error("Conch device not found: " + device.name);
        }
        backend_ = factory_.make_conch(*full);
        adapter_ = std::make_unique<ConchJobAdapter>(*backend_, full->name);
        return *adapter_;
    }

private:
    BackendFactory& factory_;
    const std::vector<ShellDeviceConfig>& devices_;
    std::unique_ptr<backends::ConchBackend> backend_;
    std::unique_ptr<ConchJobAdapter> adapter_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// ToolExecutionEvent / ToolProvider defaults
// ---------------------------------------------------------------------------------------------

ToolExecutionEvent ToolExecutionEvent::output_delta(std::string text) {
    ToolExecutionEvent event;
    event.kind = Kind::OutputDelta;
    event.text = std::move(text);
    return event;
}

ToolExecutionEvent ToolExecutionEvent::output_snapshot(std::string text) {
    ToolExecutionEvent event;
    event.kind = Kind::OutputSnapshot;
    event.text = std::move(text);
    return event;
}

ToolExecutionEvent ToolExecutionEvent::target_resolved(std::string target) {
    ToolExecutionEvent event;
    event.kind = Kind::TargetResolved;
    event.text = std::move(target);
    return event;
}

ToolExecutionEvent ToolExecutionEvent::progress(std::string message) {
    ToolExecutionEvent event;
    event.kind = Kind::Progress;
    event.text = std::move(message);
    return event;
}

ToolExecutionEvent ToolExecutionEvent::completed(ToolExecutionResult result) {
    ToolExecutionEvent event;
    event.kind = Kind::Completed;
    event.text = std::move(result.text);
    event.is_error = result.is_error;
    event.images = std::move(result.images);
    event.transcribe_images = result.transcribe_images;
    event.structured_content = std::move(result.structured_content);
    event.display_text = std::move(result.display_text);
    return event;
}

void ToolProvider::execute_events(const std::string& name, const std::string& arguments,
                                  const GenerationContext& ctx, const ToolEventSink& sink) {
    sink(ToolExecutionEvent::completed(execute_one_shot_result(name, arguments, ctx)));
}

ToolExecutionResult ToolProvider::execute_one_shot_result(const std::string& name,
                                                          const std::string& arguments,
                                                          const GenerationContext& ctx) {
    return ToolExecutionResult{execute(name, arguments, ctx)};
}

std::optional<ToolPresentationMetadata> ToolProvider::presentation_metadata(
    const std::string& /*name*/) const {
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// ShellToolProvider lifecycle and resolution helpers
// ---------------------------------------------------------------------------------------------

ShellToolProvider::ShellToolProvider(BackendFactory& backend_factory,
                                     SandboxFactory* sandbox_factory,
                                     ToolImageStore* image_store,
                                     durable_jobs::DurableJobHooks hooks)
    : backend_factory_(backend_factory),
      sandbox_factory_(sandbox_factory),
      image_store_(image_store),
      hooks_(std::move(hooks)) {
    if (sandbox_factory_ != nullptr) sandbox_ = sandbox_factory_->create();
    const durable_jobs::DurableJobHooks defaults = default_hooks();
    if (!hooks_.now) hooks_.now = defaults.now;
    if (!hooks_.delay) hooks_.delay = defaults.delay;
    if (!hooks_.is_active) hooks_.is_active = defaults.is_active;
}

std::int32_t ShellToolProvider::max_wait_ms(const GenerationContext& ctx) {
    return durable_jobs::max_wait_ms(ctx.tool_timeout_ms);
}

durable_jobs::JobToolContext ShellToolProvider::job_context(const GenerationContext& ctx) const {
    durable_jobs::JobToolContext result;
    result.tool_timeout_ms = ctx.tool_timeout_ms;
    for (const ShellDeviceConfig& device : ctx.shell_devices) {
        result.devices.push_back(shell_tools::ShellDeviceConfig{device.name, device.type});
    }
    return result;
}

const ShellDeviceConfig* ShellToolProvider::resolve_shell_device(
    const std::string& server_name, const GenerationContext& ctx) const {
    if (ascii_iequals(server_name, "Local Sandbox")) return nullptr;
    if (!json::is_blank(server_name)) {
        for (const ShellDeviceConfig& device : ctx.shell_devices) {
            if (ascii_iequals(device.name, server_name)) return &device;
        }
        return nullptr;
    }
    if (ctx.shell_devices.size() == 1) return &ctx.shell_devices.front();
    return nullptr;
}

const ShellDeviceConfig* ShellToolProvider::find_conch_device(
    const std::string& name, const GenerationContext& ctx) const {
    const ShellDeviceConfig* single = nullptr;
    std::size_t count = 0;
    for (const ShellDeviceConfig& device : ctx.shell_devices) {
        if (device.type == "ssh") continue;
        ++count;
        if (single == nullptr) single = &device;
        if (!json::is_blank(name) && ascii_iequals(device.name, name)) return &device;
    }
    if (!json::is_blank(name)) return nullptr;
    return count == 1 ? single : nullptr;
}

std::string ShellToolProvider::server_not_found_message(const std::string& server_name,
                                                        const GenerationContext& ctx) const {
    const bool has_sandbox =
        ctx.sandbox_enabled && sandbox_factory_ != nullptr && sandbox_factory_->is_available();
    std::vector<std::string> names;
    if (has_sandbox) names.emplace_back("\"Local Sandbox\"");
    for (const ShellDeviceConfig& device : ctx.shell_devices) {
        names.push_back("\"" + device.name + "\"");
    }
    if (names.size() == 1) {
        return "Unknown server: " + server_name + ". Use " + names.front() +
               " or omit the server parameter.";
    }
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += names[i];
    }
    if (json::is_blank(server_name)) {
        return "Multiple servers available. Use list_shells to see them, then specify one: " +
               joined + ".";
    }
    return "Unknown server: " + server_name + ". Available: " + joined + ".";
}

std::unique_ptr<backends::Backend> ShellToolProvider::get_backend(
    const std::string& server_name, const GenerationContext& ctx) {
    if (ascii_iequals(server_name, "Local Sandbox") && ctx.sandbox_enabled) {
        if (sandbox_ != nullptr && sandbox_->is_available()) {
            return std::make_unique<backends::SandboxBackend>(sandbox_);
        }
        if (sandbox_ != nullptr) return nullptr;
    }
    if (json::is_blank(server_name)) {
        if (ctx.sandbox_enabled && sandbox_ != nullptr && sandbox_->is_available()) {
            return std::make_unique<backends::SandboxBackend>(sandbox_);
        }
    }
    const ShellDeviceConfig* device = resolve_shell_device(server_name, ctx);
    if (device == nullptr) return nullptr;
    if (device->type == "ssh") return backend_factory_.make_ssh(*device);
    return backend_factory_.make_conch(*device);
}

std::unique_ptr<backends::ConchBackend> ShellToolProvider::get_conch_backend(
    const std::string& server_name, const GenerationContext& ctx) {
    const ShellDeviceConfig* device = find_conch_device(server_name, ctx);
    if (device == nullptr) return nullptr;
    return backend_factory_.make_conch(*device);
}

bool ShellToolProvider::confirm_target(const GenerationContext& ctx,
                                       const std::optional<ShellDeviceConfig>& device,
                                       const std::string& summary,
                                       bool local_shared_storage_exposed) {
    if (!device && !local_shared_storage_exposed) return true;
    std::string target;
    if (device) {
        target = json::is_blank(device->name) ? device->type + " server" : device->name;
    } else {
        target = "Local Sandbox \xC2\xB7 /mnt/shared";
    }
    if (!confirm_) return true;
    return confirm_(target, summary, ctx.conversation_id);
}

bool ShellToolProvider::targets_shared_storage(const std::string& path) {
    const std::string trimmed = json::trim(path);
    std::string normalized;
    normalized.reserve(trimmed.size());
    for (char c : trimmed) normalized.push_back(c == '\\' ? '/' : c);
    std::string collapsed;
    collapsed.reserve(normalized.size());
    for (char c : normalized) {
        if (c == '/' && !collapsed.empty() && collapsed.back() == '/') continue;
        collapsed.push_back(c);
    }
    return collapsed == "/mnt/shared" || collapsed.rfind("/mnt/shared/", 0) == 0;
}

// ---------------------------------------------------------------------------------------------
// ToolProvider interface
// ---------------------------------------------------------------------------------------------

std::vector<openai::ToolDefinition> ShellToolProvider::definitions(
    const GenerationContext& ctx) const {
    shell_tools::ShellDefinitionsConfig config;
    config.shell_enabled = ctx.shell_enabled;
    config.sandbox_enabled = ctx.sandbox_enabled;
    config.tool_timeout_ms = ctx.tool_timeout_ms;
    for (const ShellDeviceConfig& device : ctx.shell_devices) {
        config.devices.push_back(shell_tools::ShellDeviceConfig{device.name, device.type});
    }
    return shell_tools::build_shell_tool_definitions(config);
}

std::string ShellToolProvider::execute(const std::string& name, const std::string& arguments,
                                       const GenerationContext& ctx) {
    if (name == "list_shells") return list_shells(ctx);
    if (name == "execute_shell_command") return execute_shell_command(arguments, ctx);
    if (name == "list_shell_jobs") return list_shell_jobs(arguments, ctx);
    if (name == "get_shell_job") return get_shell_job(arguments, ctx);
    if (name == "wait_for_job") return wait_for_job(arguments, ctx);
    if (name == "stop_shell_job") return stop_shell_job(arguments, ctx);
    if (name == "file_read") return execute_file_read(arguments, ctx);
    if (name == "file_write") return execute_file_write(arguments, ctx);
    if (name == "file_edit") return execute_file_edit(arguments, ctx);
    if (name == "file_glob") return execute_file_glob(arguments, ctx);
    if (name == "file_grep") return execute_file_grep(arguments, ctx);
    if (name == "view_image") return execute_view_image(arguments, ctx).text;
    return "Unknown tool: " + name;
}

void ShellToolProvider::execute_events(const std::string& name, const std::string& arguments,
                                       const GenerationContext& ctx, const ToolEventSink& sink) {
    if (name == "execute_shell_command") {
        execute_shell_command_events(arguments, ctx, sink);
        return;
    }
    if (name == "wait_for_job") {
        wait_for_job_events(arguments, ctx, sink);
        return;
    }
    if (name == "view_image") {
        sink(ToolExecutionEvent::completed(execute_view_image(arguments, ctx)));
        return;
    }
    ToolProvider::execute_events(name, arguments, ctx, sink);
}

bool ShellToolProvider::handles(const std::string& name) const {
    return name == "list_shells" || name == "execute_shell_command" ||
           name == "list_shell_jobs" || name == "get_shell_job" || name == "wait_for_job" ||
           name == "stop_shell_job" || name == "file_read" || name == "file_write" ||
           name == "file_edit" || name == "file_glob" || name == "file_grep" ||
           name == "view_image";
}

// ---------------------------------------------------------------------------------------------
// list_shells
// ---------------------------------------------------------------------------------------------

std::string ShellToolProvider::list_shells(const GenerationContext& ctx) {
    std::vector<json::Value> devices;
    const bool sandbox_ok = ctx.sandbox_enabled && sandbox_ != nullptr && sandbox_->is_available();
    if (sandbox_ok) {
        devices.push_back(json_object({
            {"name", json_string("Local Sandbox")},
            {"description", json_string("Alpine Linux on-device")},
            {"type", json_string("local")},
        }));
    }
    for (const ShellDeviceConfig& device : ctx.shell_devices) {
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("name",
                             json_string(json::is_blank(device.name) ? "Untitled" : device.name));
        members.emplace_back("description", json_string(device.description));
        members.emplace_back("type", json_string(device.type));
        if (device.type == "ssh") {
            members.emplace_back("host", json_string(device.ssh_host));
            members.emplace_back("port", json_number(std::to_string(device.ssh_port)));
        } else {
            members.emplace_back("url", json_string(device.server_url));
        }
        devices.push_back(json_object(std::move(members)));
    }
    return json::to_kotlin_string(json_object({
        {"type", json_string("list_shells")},
        {"devices", json_array(std::move(devices))},
    }));
}

// ---------------------------------------------------------------------------------------------
// Shell execution
// ---------------------------------------------------------------------------------------------

std::string ShellToolProvider::execute_shell_command(const std::string& arguments,
                                                     const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string command = shell_tools::arg(args, "command");
    if (json::is_blank(command)) {
        return shell_tools::json_error("execute_shell_command", "no_command", std::nullopt,
                                       std::nullopt);
    }
    const std::string server_name = shell_tools::arg(args, "server");
    const std::string workdir = shell_tools::arg(args, "workdir");
    if (const auto error = shell_command_validation_error(command, workdir)) {
        return shell_tools::json_error("execute_shell_command", *error, present(server_name),
                                       std::nullopt);
    }
    const bool background = shell_tools::bool_arg(args, "background");
    const std::int32_t timeout_max = background ? kBackgroundJobMaxMs : kForegroundMaxMs;
    const std::string raw_timeout = shell_tools::arg(args, "timeout_ms");
    if (json::is_blank(raw_timeout)) {
        return shell_tools::json_error("execute_shell_command", "timeout_ms is required",
                                       present(server_name), present(command));
    }
    const auto parsed_timeout = parse_int_or_null(raw_timeout);
    if (!parsed_timeout) {
        return shell_tools::json_error(
            "execute_shell_command",
            "timeout_ms must be an integer, got \"" + raw_timeout + "\"",
            present(server_name), present(command));
    }
    const std::int32_t timeout_ms = clamp_timeout(*parsed_timeout, timeout_max);

    if (background) {
        auto backend = get_conch_backend(server_name, ctx);
        if (!backend) {
            return shell_tools::json_error(
                "execute_shell_command", durable::conch_server_not_found_message(
                                             server_name, job_context(ctx).devices),
                present(server_name), present(command));
        }
        BackendCloser closer(backend.get());
        const std::string device_name = backend->device() ? backend->device()->name : "";
        try {
            if (!confirm_target(ctx, backend->device(),
                                "start background job: $ " + command)) {
                return shell_tools::json_error(
                    "execute_shell_command",
                    "denied_by_user: the user declined to run this background command",
                    present(device_name), present(command));
            }
            return backend->start_job(command, timeout_ms, workdir);
        } catch (const durable::JobCancelled&) {
            throw;
        } catch (const std::exception& failure) {
            return shell_tools::json_error("execute_shell_command", failure.what(),
                                           present(device_name), present(command));
        }
    }

    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error("execute_shell_command",
                                       server_not_found_message(server_name, ctx), std::nullopt,
                                       std::nullopt);
    }
    BackendCloser closer(backend.get());
    if (!confirm_target(ctx, backend->device(), "$ " + command,
                        !backend->device() && ctx.sandbox_shared_storage_enabled)) {
        return shell_tools::json_error(
            "execute_shell_command", "denied_by_user: the user declined to run this command",
            present(server_name), present(command));
    }
    if (auto* conch = dynamic_cast<backends::ConchBackend*>(backend.get())) {
        ConchJobAdapter adapter(*conch, conch->device() ? conch->device()->name : "");
        const std::int32_t wait_ms = std::min(timeout_ms, max_wait_ms(ctx));
        const durable::DurableForegroundOutcome outcome =
            durable::execute_durable_foreground(command, workdir, wait_ms, adapter, hooks_);
        if (outcome.cancelled) throw durable::JobCancelled("cancelled");
        return outcome.json;
    }
    return backend->execute_command(command, workdir, timeout_ms);
}

void ShellToolProvider::execute_shell_command_events(const std::string& arguments,
                                                     const GenerationContext& ctx,
                                                     const ToolEventSink& sink) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string command = shell_tools::arg(args, "command");
    if (json::is_blank(command)) {
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            shell_tools::json_error("execute_shell_command", "no_command", std::nullopt,
                                    std::nullopt)}));
        return;
    }
    const std::string server_name = shell_tools::arg(args, "server");
    const std::string workdir = shell_tools::arg(args, "workdir");
    if (const auto error = shell_command_validation_error(command, workdir)) {
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            shell_tools::json_error("execute_shell_command", *error, present(server_name),
                                    std::nullopt)}));
        return;
    }
    if (shell_tools::bool_arg(args, "background")) {
        const ShellDeviceConfig* device = resolve_shell_device(server_name, ctx);
        if (device == nullptr || device->type == "ssh") {
            sink(ToolExecutionEvent::completed(ToolExecutionResult{
                shell_tools::json_error(
                    "execute_shell_command",
                    durable::conch_server_not_found_message(server_name,
                                                            job_context(ctx).devices),
                    present(server_name), present(command))}));
            return;
        }
        sink(ToolExecutionEvent::target_resolved(device->name));
        sink(ToolExecutionEvent::progress("Starting durable background job"));
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            execute_shell_command(arguments, ctx)}));
        return;
    }
    const std::string raw_timeout = shell_tools::arg(args, "timeout_ms");
    if (json::is_blank(raw_timeout)) {
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            shell_tools::json_error("execute_shell_command", "timeout_ms is required",
                                    present(server_name), present(command))}));
        return;
    }
    const auto parsed_timeout = parse_int_or_null(raw_timeout);
    if (!parsed_timeout) {
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            shell_tools::json_error(
                "execute_shell_command",
                "timeout_ms must be an integer, got \"" + raw_timeout + "\"",
                present(server_name), present(command))}));
        return;
    }
    const std::int32_t timeout_ms = clamp_timeout(*parsed_timeout, kForegroundMaxMs);
    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            shell_tools::json_error("execute_shell_command",
                                    server_not_found_message(server_name, ctx), std::nullopt,
                                    std::nullopt)}));
        return;
    }
    BackendCloser closer(backend.get());
    const std::optional<ShellDeviceConfig>& device = backend->device();
    const std::string target =
        device ? (json::is_blank(device->name) ? "Untitled" : device->name) : "Local Sandbox";
    sink(ToolExecutionEvent::target_resolved(target));
    if (!confirm_target(ctx, device, "$ " + command,
                        !device && ctx.sandbox_shared_storage_enabled)) {
        sink(ToolExecutionEvent::completed(ToolExecutionResult{
            shell_tools::json_error(
                "execute_shell_command", "denied_by_user: the user declined to run this command",
                present(server_name), present(command))}));
        return;
    }
    if (auto* conch = dynamic_cast<backends::ConchBackend*>(backend.get())) {
        sink(ToolExecutionEvent::progress("Running as a durable foreground job"));
        ConchJobAdapter adapter(*conch, conch->device() ? conch->device()->name : "");
        const std::int32_t wait_ms = std::min(timeout_ms, max_wait_ms(ctx));
        const durable::DurableForegroundOutcome outcome =
            durable::execute_durable_foreground(command, workdir, wait_ms, adapter, hooks_);
        for (const std::string& output : outcome.outputs) {
            sink(ToolExecutionEvent::output_delta(output));
        }
        if (outcome.cancelled) throw durable::JobCancelled("cancelled");
        sink(ToolExecutionEvent::completed(ToolExecutionResult{outcome.json}));
        return;
    }
    sink(ToolExecutionEvent::progress("Running command"));
    backend->execute_command_events(
        command, workdir, timeout_ms, [&sink](const backends::BackendEvent& event) {
            if (event.kind == backends::BackendEvent::Kind::OutputDelta) {
                sink(ToolExecutionEvent::output_delta(event.text));
            } else {
                sink(ToolExecutionEvent::completed(
                    ToolExecutionResult{.text = event.text, .is_error = event.is_error}));
            }
        });
}

// ---------------------------------------------------------------------------------------------
// Durable job tools
// ---------------------------------------------------------------------------------------------

std::string ShellToolProvider::list_shell_jobs(const std::string& arguments,
                                               const GenerationContext& ctx) {
    JobBackendFactory factory(backend_factory_, ctx.shell_devices);
    return durable::list_shell_jobs(
        arguments, job_context(ctx),
        [&factory](const shell_tools::ShellDeviceConfig& device) -> durable::DurableJobBackend& {
            return factory.make(device);
        });
}

std::string ShellToolProvider::get_shell_job(const std::string& arguments,
                                             const GenerationContext& ctx) {
    JobBackendFactory factory(backend_factory_, ctx.shell_devices);
    return durable::get_shell_job(
        arguments, job_context(ctx),
        [&factory](const shell_tools::ShellDeviceConfig& device) -> durable::DurableJobBackend& {
            return factory.make(device);
        });
}

std::string ShellToolProvider::wait_for_job(const std::string& arguments,
                                            const GenerationContext& ctx,
                                            std::vector<std::string>* snapshots) {
    JobBackendFactory factory(backend_factory_, ctx.shell_devices);
    std::vector<std::string> local;
    std::vector<std::string>& collected = snapshots != nullptr ? *snapshots : local;
    return durable::wait_for_job(
        arguments, job_context(ctx),
        [&factory](const shell_tools::ShellDeviceConfig& device) -> durable::DurableJobBackend& {
            return factory.make(device);
        },
        hooks_, collected);
}

void ShellToolProvider::wait_for_job_events(const std::string& arguments,
                                            const GenerationContext& ctx,
                                            const ToolEventSink& sink) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string server_name = shell_tools::arg(args, "server");
    if (const ShellDeviceConfig* device = find_conch_device(server_name, ctx)) {
        sink(ToolExecutionEvent::target_resolved(device->name));
    }
    sink(ToolExecutionEvent::progress("Waiting for durable job"));
    std::vector<std::string> snapshots;
    const std::string result = wait_for_job(arguments, ctx, &snapshots);
    for (const std::string& snapshot : snapshots) {
        sink(ToolExecutionEvent::output_snapshot(snapshot));
    }
    sink(ToolExecutionEvent::completed(ToolExecutionResult{result}));
}

std::string ShellToolProvider::stop_shell_job(const std::string& arguments,
                                              const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string job_id = shell_tools::arg(args, "job_id");
    if (json::is_blank(job_id)) {
        return shell_tools::json_error("stop_shell_job", "job_id is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string server_name = shell_tools::arg(args, "server");
    auto backend = get_conch_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error(
            "stop_shell_job",
            durable::conch_server_not_found_message(server_name, job_context(ctx).devices),
            present(server_name), std::nullopt);
    }
    BackendCloser closer(backend.get());
    const std::string device_name = backend->device() ? backend->device()->name : "";
    if (!confirm_target(ctx, backend->device(), "stop background shell job: " + job_id)) {
        return shell_tools::json_error(
            "stop_shell_job", "denied_by_user: the user declined to stop this job",
            present(device_name), std::nullopt);
    }
    try {
        return backend->stop_job(job_id, std::nullopt);
    } catch (const durable::JobCancelled&) {
        throw;
    } catch (const std::exception& failure) {
        return shell_tools::json_error("stop_shell_job", failure.what(), present(device_name),
                                       std::nullopt);
    }
}

// ---------------------------------------------------------------------------------------------
// File tools
// ---------------------------------------------------------------------------------------------

ToolExecutionResult ShellToolProvider::execute_view_image(const std::string& arguments,
                                                          const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string path = shell_tools::arg(args, "path");
    if (json::is_blank(path)) {
        return ToolExecutionResult{
            .text = shell_tools::json_error("view_image", "path is required", std::nullopt,
                                            std::nullopt),
            .is_error = true};
    }
    const std::string server_name = shell_tools::arg(args, "server");
    if (image_store_ == nullptr) {
        return ToolExecutionResult{
            .text = shell_tools::json_error("view_image", "Tool image storage is unavailable",
                                            present(server_name), std::nullopt),
            .is_error = true};
    }
    auto backend = get_conch_backend(server_name, ctx);
    if (!backend) {
        return ToolExecutionResult{
            .text = shell_tools::json_error(
                "view_image",
                durable::conch_server_not_found_message(server_name, job_context(ctx).devices),
                present(server_name), std::nullopt),
            .is_error = true};
    }
    BackendCloser closer(backend.get());
    const std::string device_name = backend->device() ? backend->device()->name : "";
    try {
        const client::ShellClient::FileImageResult remote = backend->view_image(path);
        if (remote.error) {
            return ToolExecutionResult{
                .text = shell_tools::json_error("view_image", *remote.error, present(device_name),
                                                std::nullopt),
                .is_error = true};
        }
        const ToolImageAttachment attachment = image_store_->persist_base64(
            remote.data, remote.mime_type, "conch");
        std::vector<std::pair<std::string, json::Value>> members;
        members.emplace_back("type", json_string("view_image"));
        members.emplace_back("server", json_string(device_name));
        members.emplace_back("path", json_string(path));
        members.emplace_back("mime_type", json_string(attachment.mime_type));
        members.emplace_back("size", json_number(std::to_string(attachment.size_bytes)));
        if (attachment.width) {
            members.emplace_back("width", json_number(std::to_string(*attachment.width)));
        }
        if (attachment.height) {
            members.emplace_back("height", json_number(std::to_string(*attachment.height)));
        }
        members.emplace_back("ok", json_bool(true));
        ToolExecutionResult result;
        result.text = json::to_kotlin_string(json_object(std::move(members)));
        result.images.push_back(attachment);
        result.transcribe_images = true;
        return result;
    } catch (const std::exception& error) {
        return ToolExecutionResult{
            .text = shell_tools::json_error("view_image", error.what(), present(device_name),
                                            std::nullopt),
            .is_error = true};
    }
}

std::string ShellToolProvider::execute_file_read(const std::string& arguments,
                                                 const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string path = shell_tools::arg(args, "path");
    if (json::is_blank(path)) {
        return shell_tools::json_error("file_read", "path is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string server_name = shell_tools::arg(args, "server");
    const std::int64_t offset = parse_long_or_null(shell_tools::arg(args, "offset")).value_or(0);
    const std::int64_t limit = parse_long_or_null(shell_tools::arg(args, "limit")).value_or(0);

    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error("file_read", server_not_found_message(server_name, ctx),
                                       std::nullopt, std::nullopt);
    }
    BackendCloser closer(backend.get());
    const std::string server = backend->device() ? backend->device()->name : "Local Sandbox";
    shell_tools::ShellFileReadResult result;
    try {
        result = backend->file_read(path, offset, limit);
    } catch (const durable::JobCancelled&) {
        throw;
    } catch (const std::exception& error) {
        return shell_tools::json_error("file_read", error.what(), present(server),
                                       std::nullopt);
    }
    if (result.error) {
        return shell_tools::json_error("file_read", *result.error, present(server),
                                       std::nullopt);
    }
    return shell_tools::bounded_file_read_json(server, path, result);
}

std::string ShellToolProvider::execute_file_write(const std::string& arguments,
                                                  const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string path = shell_tools::arg(args, "path");
    if (json::is_blank(path)) {
        return shell_tools::json_error("file_write", "path is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string content = shell_tools::arg(args, "content");
    if (json::is_blank(content)) {
        return shell_tools::json_error("file_write", "content is required", std::nullopt,
                                       std::nullopt);
    }
    if (content.size() > kFileWriteMaxBytes) {
        return shell_tools::json_error("file_write", "content exceeds 1MB limit", std::nullopt,
                                       std::nullopt);
    }
    const std::string server_name = shell_tools::arg(args, "server");

    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error("file_write", server_not_found_message(server_name, ctx),
                                       std::nullopt, std::nullopt);
    }
    BackendCloser closer(backend.get());
    if (!confirm_target(ctx, backend->device(), "write file: " + path,
                        !backend->device() && targets_shared_storage(path))) {
        return shell_tools::json_error(
            "file_write", "denied_by_user: the user declined to write this file",
            present(server_name), std::nullopt);
    }
    const std::optional<std::string> error = backend->file_write(path, content);
    if (error) return *error;
    return json::to_kotlin_string(json_object({
        {"type", json_string("file_write")},
        {"path", json_string(path)},
        {"ok", json_bool(true)},
    }));
}

std::string ShellToolProvider::execute_file_edit(const std::string& arguments,
                                                 const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string path = shell_tools::arg(args, "path");
    if (json::is_blank(path)) {
        return shell_tools::json_error("file_edit", "path is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string old_string = shell_tools::arg(args, "old_string");
    if (json::is_blank(old_string)) {
        return shell_tools::json_error("file_edit", "old_string is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string new_string = shell_tools::arg(args, "new_string");
    const bool replace_all = ascii_iequals(shell_tools::arg(args, "replace_all"), "true");
    const std::string server_name = shell_tools::arg(args, "server");

    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error("file_edit", server_not_found_message(server_name, ctx),
                                       std::nullopt, std::nullopt);
    }
    BackendCloser closer(backend.get());
    if (!confirm_target(ctx, backend->device(), "edit file: " + path,
                        !backend->device() && targets_shared_storage(path))) {
        return shell_tools::json_error(
            "file_edit", "denied_by_user: the user declined to edit this file",
            present(server_name), std::nullopt);
    }
    const std::string server = backend->device() ? backend->device()->name : "Local Sandbox";
    shell_file::ShellFileEditResult result;
    try {
        result = backend->file_edit(path, old_string, new_string, replace_all);
    } catch (const durable::JobCancelled&) {
        throw;
    } catch (const std::exception& error) {
        return shell_tools::json_error("file_edit", error.what(), present(server),
                                       std::nullopt);
    }
    if (result.error) {
        return shell_tools::json_error("file_edit", *result.error, present(server),
                                       std::nullopt);
    }
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("type", json_string("file_edit"));
    members.emplace_back("server", json_string(server));
    members.emplace_back("path", json_string(path));
    members.emplace_back("replaced", json_number(std::to_string(result.replacements)));
    if (!json::is_blank(result.sha256)) {
        members.emplace_back("sha256", json_string(result.sha256));
    }
    return json::to_kotlin_string(json_object(std::move(members)));
}

std::string ShellToolProvider::execute_file_glob(const std::string& arguments,
                                                 const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string pattern = shell_tools::arg(args, "pattern");
    if (json::is_blank(pattern)) {
        return shell_tools::json_error("file_glob", "pattern is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string server_name = shell_tools::arg(args, "server");
    const std::string base_path = shell_tools::arg(args, "path");
    const std::optional<std::int32_t> depth =
        parse_int_or_null(shell_tools::arg(args, "depth"));

    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error("file_glob", server_not_found_message(server_name, ctx),
                                       std::nullopt, std::nullopt);
    }
    BackendCloser closer(backend.get());
    const backends::GlobOutcome outcome = backend->file_glob(pattern, base_path, depth);
    if (!outcome.ok) {
        return shell_tools::json_error(
            "file_glob", outcome.error.empty() ? "Unknown error" : outcome.error, std::nullopt,
            std::nullopt);
    }
    std::vector<json::Value> files;
    files.reserve(outcome.files.size());
    for (const std::string& file : outcome.files) files.push_back(json_string(file));
    return json::to_kotlin_string(json_object({
        {"type", json_string("file_glob")},
        {"pattern", json_string(pattern)},
        {"files", json_array(std::move(files))},
        {"truncated", json_bool(outcome.truncated)},
    }));
}

std::string ShellToolProvider::execute_file_grep(const std::string& arguments,
                                                 const GenerationContext& ctx) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string pattern = shell_tools::arg(args, "pattern");
    if (json::is_blank(pattern)) {
        return shell_tools::json_error("file_grep", "pattern is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string server_name = shell_tools::arg(args, "server");
    const std::string base_path = shell_tools::arg(args, "path");
    const std::string file_glob = shell_tools::arg(args, "glob");

    auto backend = get_backend(server_name, ctx);
    if (!backend) {
        return shell_tools::json_error("file_grep", server_not_found_message(server_name, ctx),
                                       std::nullopt, std::nullopt);
    }
    BackendCloser closer(backend.get());
    const backends::GrepOutcome outcome = backend->file_grep(pattern, base_path, file_glob);
    if (!outcome.ok) {
        return shell_tools::json_error(
            "file_grep", outcome.error.empty() ? "Unknown error" : outcome.error, std::nullopt,
            std::nullopt);
    }
    std::vector<json::Value> matches;
    matches.reserve(outcome.matches.size());
    for (const backends::GrepMatch& match : outcome.matches) {
        matches.push_back(json_object({
            {"path", json_string(match.path)},
            {"line", json_number(std::to_string(match.line))},
            {"content", json_string(match.content)},
        }));
    }
    return json::to_kotlin_string(json_object({
        {"type", json_string("file_grep")},
        {"pattern", json_string(pattern)},
        {"matches", json_array(std::move(matches))},
        {"truncated", json_bool(outcome.truncated)},
    }));
}

// ---------------------------------------------------------------------------------------------
// acknowledgeCommittedJobs
// ---------------------------------------------------------------------------------------------

void ShellToolProvider::acknowledge_committed_jobs(
    const std::vector<durable_jobs::ToolCallRecord>& calls, const GenerationContext& ctx) {
    const std::vector<durable_jobs::JobAcknowledgement> acknowledgements =
        durable_jobs::terminal_shell_job_acknowledgements(calls);
    for (const durable_jobs::JobAcknowledgement& acknowledgement : acknowledgements) {
        auto backend = get_conch_backend(acknowledgement.server_name, ctx);
        if (!backend) continue;
        BackendCloser closer(backend.get());
        try {
            backend->acknowledge_job(acknowledgement.job_id);
        } catch (const durable::JobCancelled&) {
            throw;
        } catch (...) {
            // Retention TTL/count remains the safe fallback when cleanup is offline.
        }
    }
}

}  // namespace agora::core::shell_provider
