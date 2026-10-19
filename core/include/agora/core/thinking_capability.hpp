#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::thinking_capability {

/**
 * Port of the zero-import thinking-capability cluster:
 * model/ModelThinkingCapability.kt, model/ThinkingControl.kt,
 * model/ModelThinkingCapabilityDefaults.kt, model/ModelThinkingCapabilities.kt.
 *
 * Boundary note: `ThinkingLevels.normalize`/`indexForEffort` call Kotlin `String.lowercase()`,
 * which is Unicode-aware. This port folds ASCII A-Z only. Every key the cluster compares against
 * is ASCII (family names, protocol names, model-id prefixes), so the distinction is observable
 * only for exotic non-ASCII spellings that cannot match any table entry anyway; `trim()` is
 * Unicode-aware and uses the JVM whitespace table like the Kotlin original.
 */

inline constexpr std::string_view kDefaultEffort = "medium";
inline constexpr int kDefaultBudgetTokens = 4096;

/** Ordered effort vocabulary (ThinkingLevels.effortValues). */
const std::vector<std::string>& effort_values();

/** Budget presets exposed by the UI (ThinkingLevels.budgetPresets). */
const std::vector<int>& budget_presets();

/** Wire families that need different thinking parameters. */
enum class ThinkingProviderFamily {
    Anthropic,
    OpenAI,
    OpenAICompatible,
    OpenRouter,
    DeepSeek,
    Qwen,
    Groq,
    Gemini,
    Ollama,
    Local,
};

/** What a single model can do with thinking/reasoning parameters. */
struct Capability {
    bool can_disable_thinking = true;
    std::vector<std::string> supported_efforts = effort_values();
    bool supports_thinking_budget = true;
    int min_budget_tokens = 1;
    std::optional<int> max_budget_tokens;
    bool supports_effort_with_budget = true;
    bool supports_sampling_params = true;

    bool supports_effort() const { return !supported_efforts.empty(); }
    std::optional<std::string> lowest_effort() const;
};

/** The permissive superset used for every undocumented model. */
Capability permissive();

/** Kotlin ThinkingLevels.normalize. */
std::string normalize(const std::optional<std::string>& raw);

/** Kotlin ThinkingLevels.legacyBudgetTokens: `budget:<positive Int>`, else nullopt. */
std::optional<int> legacy_budget_tokens(const std::optional<std::string>& raw);

/** Kotlin ThinkingLevels.effortForIndex: out-of-range indices fall back to "medium". */
std::string effort_for_index(int index);

/** Kotlin ThinkingLevels.indexForEffort: unknown values coerce to 0. */
int index_for_effort(std::string_view effort);

/** ModelThinkingCapabilities.familyFor: custom protocol wins over the display name. */
ThinkingProviderFamily family_for(const std::optional<std::string>& provider_name,
                                  const std::optional<std::string>& custom_protocol);

/** ModelThinkingCapabilityDefaults.familyDefault. */
Capability family_default(ThinkingProviderFamily family);

/** ModelThinkingCapabilityDefaults.forModel: family default plus documented refinements. */
Capability for_model(ThinkingProviderFamily family, std::string_view model_id);

/** ModelThinkingCapabilities.forProvider: family resolution followed by forModel. */
Capability for_provider(const std::optional<std::string>& provider_name, std::string_view model_id,
                        const std::optional<std::string>& custom_protocol);

/** Nearest accepted effort; ties resolve upward. nullopt when there is no selector. */
std::optional<std::string> nearest_effort(const Capability& capability, std::string_view effort);

/** Kotlin coerceIn(minBudgetTokens, maxBudgetTokens ?: Int.MAX_VALUE). */
int clamp_budget(const Capability& capability, int tokens);

/** The thinking configuration that will actually be sent. */
struct ResolvedThinking {
    bool enabled = false;
    std::optional<std::string> effort;
    std::optional<int> budget_tokens;
    Capability capability;
};

/** ThinkingResolution.resolve. */
ResolvedThinking resolve(const Capability& capability, bool requested_enabled,
                         std::string_view requested_effort, bool requested_budget_enabled,
                         int requested_budget_tokens);

/** Kotlin enum `name` and data-class `toString()` reproductions for the golden traces. */
std::string describe(ThinkingProviderFamily family);
std::string describe(const Capability& capability);
std::string describe(const ResolvedThinking& resolved);

}  // namespace agora::core::thinking_capability
