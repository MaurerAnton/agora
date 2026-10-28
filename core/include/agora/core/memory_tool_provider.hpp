#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/shell_tool_provider.hpp"

// tool/MemoryToolProvider.kt: the memory-file and active-memory tools over MemoryManager.
// Two context gates (saved files vs active memory) publish disjoint tool sets; execution
// overrides executeOneShotResult (isError envelopes), and execute() is its .text adapter.

namespace agora::core::memory {

/** MemoryManager's file list rows. */
struct MemoryFile {
    std::string name;
    std::string description;
};

/**
 * The MemoryManager seam (data/MemoryManager.kt). Kotlin's editFile overloads (content /
 * oldString+newString / newName / description named arguments) collapse into one request with
 * whichever fields the dispatch set.
 */
struct EditFileRequest {
    std::string name;
    std::optional<std::string> content;      // operation=replace
    std::optional<std::string> old_string;   // operation=patch
    std::optional<std::string> new_string;   // operation=patch
    std::optional<std::string> new_name;     // operation=rename
    std::optional<std::string> description;  // operation=describe
};

class MemorySeam {
public:
    virtual ~MemorySeam() = default;

    virtual std::vector<MemoryFile> list_files() = 0;
    virtual std::string read_file(const std::string& name) = 0;
    virtual std::string create_file(const std::string& name, const std::string& content,
                                    const std::string& description) = 0;
    virtual std::string edit_file(const EditFileRequest& request) = 0;
    virtual std::string delete_file(const std::string& name) = 0;
    virtual std::string get_active_memory() = 0;
    virtual std::string update_active_memory(const std::string& content, const std::string& mode,
                                             const std::optional<std::string>& old_string,
                                             const std::optional<std::string>& new_string) = 0;
};

class MemoryToolProvider final : public shell_provider::ToolProvider {
public:
    explicit MemoryToolProvider(MemorySeam& memory);

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    /** Kotlin execute: `executeOneShotResult(...).text`. */
    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    /** The real dispatch: ToolExecutionResult envelopes (isError on the validation paths). */
    shell_provider::ToolExecutionResult execute_one_shot_result(
        const std::string& name, const std::string& arguments,
        const shell_provider::GenerationContext& ctx) override;

    /** Kotlin handles(): the seven names, ungated (definitions carry the gates). */
    bool handles(const std::string& name) const override;

private:
    MemorySeam& memory_;
};

}  // namespace agora::core::memory