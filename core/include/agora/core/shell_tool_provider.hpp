#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/openai_request.hpp"
#include "agora/core/shell_durable_jobs.hpp"
#include "agora/core/shell_tool_backends.hpp"

namespace agora::core::shell_provider {

// tool/ShellToolProvider.kt: the dispatch layer between the generation pipeline and the pinned
// shell backends. It owns target resolution, the shared-storage confirmation gate, the tool
// argument validation, the Conch durable-job lifecycle, the file tool JSON envelopes and the
// optional image persistence.
//
// The Kotlin provider constructs ConchBackend/SshBackend directly from ShellDeviceConfig and
// reads the local SandboxManager from SandboxManagerFactory. The port keeps the same shape but
// makes construction a platform seam (BackendFactory) so the golden stand can inject the
// scripted Conch/SSH transports, and exposes SandboxManagerFactory and ToolImageStore as
// interfaces. Coroutine cancellation and Dispatchers.IO remain platform concerns: the port is
// synchronous, and the durable-job wait loop takes its clock/delay/is-active hooks from
// durable_jobs::DurableJobHooks.

using ShellDeviceConfig = shell_backends::ShellDeviceConfig;

/** The GenerationContext fields ShellToolProvider.kt / WebSearchToolProvider.kt read. */
struct GenerationContext {
    std::optional<std::string> conversation_id;
    bool shell_enabled = false;
    std::vector<ShellDeviceConfig> shell_devices;
    bool sandbox_enabled = false;
    bool sandbox_shared_storage_enabled = false;
    std::int64_t tool_timeout_ms = 60'000;

    // WebSearchToolProvider.kt (GenerationContracts.kt webSearch fields).
    bool web_search_enabled = false;
    std::map<std::string, std::string> web_search_api_keys;
    std::string web_search_provider = "duckduckgo";
    std::int32_t web_search_num_results = 5;
    std::string web_search_base_url;

    // AskUserToolProvider.kt (GenerationContracts.kt askUserEnabled).
    bool ask_user_enabled = false;
};

/** com.newoether.agora.model.ToolImageAttachment. */
struct ToolImageAttachment {
    std::string path;
    std::string mime_type;
    std::int64_t size_bytes = 0;
    std::optional<std::int32_t> width;
    std::optional<std::int32_t> height;
    std::string sha256;
};

/** Provider-neutral tool output (ToolExecutionResult in ToolProvider.kt). */
struct ToolExecutionResult {
    std::string text;
    std::vector<ToolImageAttachment> images;
    std::optional<std::string> structured_content;
    std::optional<std::string> display_text;
    bool is_error = false;
    bool transcribe_images = false;
};

/** ToolExecutionEvent in ToolProvider.kt. */
struct ToolExecutionEvent {
    enum class Kind { OutputDelta, OutputSnapshot, TargetResolved, Progress, Completed };

    Kind kind = Kind::Completed;
    std::string text;  // delta/snapshot text, target, progress message or completed text
    bool is_error = false;
    std::vector<ToolImageAttachment> images;
    bool transcribe_images = false;
    std::optional<std::string> structured_content;
    std::optional<std::string> display_text;

    static ToolExecutionEvent output_delta(std::string text);
    static ToolExecutionEvent output_snapshot(std::string text);
    static ToolExecutionEvent target_resolved(std::string target);
    static ToolExecutionEvent progress(std::string message);
    static ToolExecutionEvent completed(ToolExecutionResult result);
};

using ToolEventSink = std::function<void(const ToolExecutionEvent&)>;

/** ToolPresentationMetadata in ToolProvider.kt. */
struct ToolPresentationMetadata {
    std::string display_name;
    std::optional<std::string> target;
};

/** The ToolProvider.kt contract (the provider-neutral surface the generation pipeline drives). */
class ToolProvider {
public:
    virtual ~ToolProvider() = default;

    virtual std::vector<openai::ToolDefinition> definitions(const GenerationContext& ctx) const = 0;
    virtual std::string execute(const std::string& name, const std::string& arguments,
                                const GenerationContext& ctx) = 0;
    /** Default: one Completed carrying execute_one_shot_result(). */
    virtual void execute_events(const std::string& name, const std::string& arguments,
                                const GenerationContext& ctx, const ToolEventSink& sink);
    /** Default: ToolExecutionResult(text = execute()). */
    virtual ToolExecutionResult execute_one_shot_result(const std::string& name,
                                                        const std::string& arguments,
                                                        const GenerationContext& ctx);
    virtual bool handles(const std::string& name) const = 0;
    /** Default: nullopt (the shell provider leaves UI naming on the localized path). */
    virtual std::optional<ToolPresentationMetadata> presentation_metadata(
        const std::string& name) const;
};

// ---------------------------------------------------------------------------------------------
// Platform seams
// ---------------------------------------------------------------------------------------------

/**
 * Constructs the per-call backends exactly where ShellToolProvider.kt writes
 * `ConchBackend(device)` / `SshBackend(device)`. The real factory owns the OkHttp/JSch
 * transports; the stand injects the scripted doubles.
 */
class BackendFactory {
public:
    virtual ~BackendFactory() = default;
    virtual std::unique_ptr<shell_backends::ConchBackend> make_conch(
        const ShellDeviceConfig& device) = 0;
    virtual std::unique_ptr<shell_backends::SshBackend> make_ssh(
        const ShellDeviceConfig& device) = 0;
};

/**
 * sandbox/SandboxManagerFactory.kt. The provider calls create() once at construction (like the
 * Kotlin `private val sandbox = sandboxFactory?.create()`) and consults isAvailable() when it
 * builds the server-not-found message.
 */
class SandboxFactory {
public:
    virtual ~SandboxFactory() = default;
    virtual shell_backends::SandboxManager* create() = 0;
    virtual bool is_available() = 0;
};

/** tool/ToolImageStore.kt's persistBase64 (Android Bitmap/Context work stays on the platform). */
class ToolImageStore {
public:
    virtual ~ToolImageStore() = default;
    virtual ToolImageAttachment persist_base64(const std::string& data,
                                               const std::string& mime_type,
                                               const std::string& file_prefix) = 0;
};

/** The confirmation gate (`confirm: (suspend (server, summary, conversationId) -> Boolean)?`). */
using ConfirmCallback =
    std::function<bool(const std::string& server, const std::string& summary,
                       const std::optional<std::string>& conversation_id)>;

// ---------------------------------------------------------------------------------------------
// ShellToolProvider
// ---------------------------------------------------------------------------------------------

class ShellToolProvider final : public ToolProvider {
public:
    /**
     * @param backend_factory  required backend construction seam;
     * @param sandbox_factory  nullable, exactly like the Kotlin constructor default;
     * @param image_store      nullable, exactly like the Kotlin constructor default;
     * @param hooks            durable-job clock/delay/is-active hooks (defaults: system clock,
     *                         blocking sleep, always active).
     */
    ShellToolProvider(BackendFactory& backend_factory, SandboxFactory* sandbox_factory = nullptr,
                      ToolImageStore* image_store = nullptr,
                      durable_jobs::DurableJobHooks hooks = {});

    void set_confirm(ConfirmCallback confirm) { confirm_ = std::move(confirm); }
    const ConfirmCallback& confirm() const noexcept { return confirm_; }

    std::vector<openai::ToolDefinition> definitions(const GenerationContext& ctx) const override;
    std::string execute(const std::string& name, const std::string& arguments,
                        const GenerationContext& ctx) override;
    void execute_events(const std::string& name, const std::string& arguments,
                        const GenerationContext& ctx, const ToolEventSink& sink) override;
    bool handles(const std::string& name) const override;

    /**
     * Best-effort cleanup after the caller committed these exact results to Room. Kotlin runs the
     * acknowledgements concurrently under withTimeoutOrNull(3s); the synchronous port issues them
     * sequentially and leaves the cancellation budget to the transport.
     */
    void acknowledge_committed_jobs(const std::vector<durable_jobs::ToolCallRecord>& calls,
                                    const GenerationContext& ctx);

    /** ShellDurableJobExecutor.maxWaitMs(ctx) (also used by the wait_for_job description). */
    static std::int32_t max_wait_ms(const GenerationContext& ctx);

private:
    const ShellDeviceConfig* resolve_shell_device(const std::string& server_name,
                                                  const GenerationContext& ctx) const;
    std::string server_not_found_message(const std::string& server_name,
                                         const GenerationContext& ctx) const;
    std::unique_ptr<shell_backends::Backend> get_backend(const std::string& server_name,
                                                         const GenerationContext& ctx);
    std::unique_ptr<shell_backends::ConchBackend> get_conch_backend(
        const std::string& server_name, const GenerationContext& ctx);

    bool confirm_target(const GenerationContext& ctx,
                        const std::optional<ShellDeviceConfig>& device,
                        const std::string& summary, bool local_shared_storage_exposed = false);
    static bool targets_shared_storage(const std::string& path);

    std::string list_shells(const GenerationContext& ctx);
    std::string execute_shell_command(const std::string& arguments,
                                      const GenerationContext& ctx);
    void execute_shell_command_events(const std::string& arguments,
                                      const GenerationContext& ctx, const ToolEventSink& sink);
    void wait_for_job_events(const std::string& arguments, const GenerationContext& ctx,
                             const ToolEventSink& sink);

    std::string list_shell_jobs(const std::string& arguments, const GenerationContext& ctx);
    std::string get_shell_job(const std::string& arguments, const GenerationContext& ctx);
    std::string wait_for_job(const std::string& arguments, const GenerationContext& ctx,
                             std::vector<std::string>* snapshots = nullptr);
    std::string stop_shell_job(const std::string& arguments, const GenerationContext& ctx);

    ToolExecutionResult execute_view_image(const std::string& arguments,
                                           const GenerationContext& ctx);
    std::string execute_file_read(const std::string& arguments, const GenerationContext& ctx);
    std::string execute_file_write(const std::string& arguments, const GenerationContext& ctx);
    std::string execute_file_edit(const std::string& arguments, const GenerationContext& ctx);
    std::string execute_file_glob(const std::string& arguments, const GenerationContext& ctx);
    std::string execute_file_grep(const std::string& arguments, const GenerationContext& ctx);

    durable_jobs::JobToolContext job_context(const GenerationContext& ctx) const;
    const ShellDeviceConfig* find_conch_device(const std::string& name,
                                               const GenerationContext& ctx) const;

    BackendFactory& backend_factory_;
    SandboxFactory* sandbox_factory_ = nullptr;
    shell_backends::SandboxManager* sandbox_ = nullptr;
    ToolImageStore* image_store_ = nullptr;
    durable_jobs::DurableJobHooks hooks_;
    ConfirmCallback confirm_;
};

}  // namespace agora::core::shell_provider
