#pragma once

#include <string>
#include <vector>

#include "agora/core/shell_tool_provider.hpp"
#include "agora/core/skill_manager.hpp"

// tool/SkillToolProvider.kt: the five skill-file tools over SkillManager. Two context gates
// split the definitions into read (list/read) and modify (create/edit/delete) sets; the
// execution-time gate re-checks access BEFORE decoding arguments, and every failure is an
// isError ToolExecutionResult (execute() is its .text adapter). Manager exceptions (missing
// files, duplicate names) propagate like the Kotlin's withContext body, which has no catch.

namespace agora::core::skill {

class SkillToolProvider final : public shell_provider::ToolProvider {
public:
    explicit SkillToolProvider(SkillManager& skills);

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    /** Kotlin execute: `executeOneShotResult(...).text`. */
    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

    /** Kotlin executeOneShotResult: the isError envelopes live here. */
    shell_provider::ToolExecutionResult execute_one_shot_result(
        const std::string& name, const std::string& arguments,
        const shell_provider::GenerationContext& ctx) override;

    bool handles(const std::string& name) const override;

private:
    SkillManager& skills_;
};

}  // namespace agora::core::skill
