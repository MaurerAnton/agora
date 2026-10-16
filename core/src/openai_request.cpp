// The OpenAI chat-completions request pipeline, ported from the Kotlin app sources.
//
// Covers, in the factory's step order: MessageConverter (conversation -> wire messages),
// the thinking-resolution machinery (ModelThinkingCapability* + ThinkingControl), the six
// provider customizeRequest hooks (OpenAiThinkingHooks), Base64FileRegistry (placeholder
// projection and the replayable write loop), the two wire gates (OpenAiChatRequestValidator +
// RequestFormatValidation) and OpenAiChatRequestFactory itself. Every branch is pinned by the
// golden traces under core/tools/golden/openai-request and must stay byte-identical.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "agora/core/openai_request.hpp"
#include "agora/core/sha256.hpp"

namespace agora::core::openai {
namespace {

namespace json = agora::core::json;

constexpr std::string_view kToolMsgPrefix = "tool_";
constexpr std::string_view kResultMsgPrefix = "result_";
constexpr std::string_view kDefaultEffort = "medium";
/** The harness-side default relay endpoint; providers pass their own defaultBaseUrl. */
constexpr std::string_view kDefaultCustomBaseUrl = "https://relay.example/v1";

[[noreturn]] void throw_illegal_argument(const std::string& what) {
    throw ProviderRequestError{"IllegalArgumentException", what};
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string to_hex_lower(const std::uint8_t* bytes, std::size_t count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

// -- ThinkingLevels -----------------------------------------------------------------------------

/** ThinkingLevels.normalizeEffort: the legacy alias table. */
std::string normalize_effort_alias(const std::string& raw) {
    const std::string value = lower_ascii(json::trim(raw));
    if (value == "none") return "none";
    if (value == "minimal" || value == "min") return "minimal";
    if (value == "low" || value == "quick" || value == "fast") return "low";
    if (value == "medium" || value == "balanced" || value == "balance") return "medium";
    if (value == "high" || value == "deep") return "high";
    if (value == "xhigh" || value == "x-high" || value == "extra-high" || value == "extreme") {
        return "xhigh";
    }
    if (value == "max" || value == "max") return "max";
    return "medium";
}

// -- ModelThinkingCapabilityDefaults -------------------------------------------------------------
// Kotlin `copy` helper: start from a base capability and override the named fields. Every field
// carries an explicit "unset" default so designated initializers stay warning-clean.
struct CapabilitySpec {
    std::optional<bool> can_disable_thinking = std::nullopt;
    std::optional<std::vector<std::string>> supported_efforts = std::nullopt;
    std::optional<bool> supports_thinking_budget = std::nullopt;
    std::optional<std::int32_t> min_budget_tokens = std::nullopt;
    bool clear_max_budget_tokens = false;
    std::optional<std::int32_t> max_budget_tokens = std::nullopt;
    std::optional<bool> supports_effort_with_budget = std::nullopt;
    std::optional<bool> supports_sampling_params = std::nullopt;
};

ModelThinkingCapability derive(const ModelThinkingCapability& base, const CapabilitySpec& spec) {
    ModelThinkingCapability result = base;
    if (spec.can_disable_thinking) result.can_disable_thinking = *spec.can_disable_thinking;
    if (spec.supported_efforts) result.supported_efforts = *spec.supported_efforts;
    if (spec.supports_thinking_budget) result.supports_thinking_budget = *spec.supports_thinking_budget;
    if (spec.min_budget_tokens) result.min_budget_tokens = *spec.min_budget_tokens;
    if (spec.clear_max_budget_tokens) result.max_budget_tokens.reset();
    if (spec.max_budget_tokens) result.max_budget_tokens = *spec.max_budget_tokens;
    if (spec.supports_effort_with_budget) {
        result.supports_effort_with_budget = *spec.supports_effort_with_budget;
    }
    if (spec.supports_sampling_params) result.supports_sampling_params = *spec.supports_sampling_params;
    return result;
}

ModelThinkingCapability make_anthropic() {
    ModelThinkingCapability capability;
    capability.supported_efforts = {"low", "medium", "high", "xhigh", "max"};
    capability.min_budget_tokens = 1024;
    capability.supports_effort_with_budget = false;
    capability.supports_sampling_params = false;
    return capability;
}

ModelThinkingCapability make_open_ai() {
    ModelThinkingCapability capability;  // efforts = effortValues
    capability.supports_thinking_budget = false;
    return capability;
}

ModelThinkingCapability make_open_router() {
    ModelThinkingCapability capability;
    capability.supports_thinking_budget = true;
    capability.supports_effort_with_budget = false;
    return capability;
}

ModelThinkingCapability make_deep_seek() {
    ModelThinkingCapability capability;
    capability.supported_efforts = {"low", "high", "max"};
    capability.supports_thinking_budget = false;
    return capability;
}

ModelThinkingCapability make_qwen_hybrid() {
    ModelThinkingCapability capability;
    capability.supported_efforts.clear();
    capability.supports_thinking_budget = true;
    capability.min_budget_tokens = 1;
    capability.max_budget_tokens = 32768;
    return capability;
}

ModelThinkingCapability make_qwen38_effort() {
    ModelThinkingCapability capability;
    capability.supported_efforts = {"low", "medium", "xhigh"};
    capability.supports_thinking_budget = true;
    capability.min_budget_tokens = 1;
    capability.max_budget_tokens = 32768;
    capability.supports_effort_with_budget = false;
    return capability;
}

ModelThinkingCapability make_groq() {
    ModelThinkingCapability capability;
    capability.supported_efforts = {"low", "medium", "high"};
    capability.supports_thinking_budget = false;
    return capability;
}

ModelThinkingCapability make_gemini() {
    ModelThinkingCapability capability;
    capability.supported_efforts = {"minimal", "low", "medium", "high"};
    capability.supports_thinking_budget = true;
    capability.min_budget_tokens = 1;
    capability.supports_effort_with_budget = false;
    return capability;
}

ModelThinkingCapability make_ollama() {
    ModelThinkingCapability capability;
    capability.supported_efforts.clear();
    capability.supports_thinking_budget = false;
    return capability;
}

const std::vector<std::string>& qwen_thinking_only_models() {
    static const std::vector<std::string> kModels = {
        "qwen3.7-max-preview", "qwen3.7-max-2026-05-17", "qwen3-next-80b-a3b-thinking",
        "qwen3-235b-a22b-thinking-2507", "qwen3-30b-a3b-thinking-2507", "qwq-plus",
    };
    return kModels;
}

/** anthropicCapability: generation markers pick which thinking parameters exist at all. */
ModelThinkingCapability anthropic_capability(const std::string& model) {
    const ModelThinkingCapability anthropic = make_anthropic();
    const ModelThinkingCapability anthropic_undocumented =
        derive(ModelThinkingCapability::permissive(), CapabilitySpec{.min_budget_tokens = 1024});

    if (!starts_with(model, "claude")) return anthropic_undocumented;

    const auto any_starts_with = [&](std::initializer_list<std::string_view> prefixes) {
        for (std::string_view prefix : prefixes) {
            if (starts_with(model, prefix)) return true;
        }
        return false;
    };
    if (any_starts_with({"claude-3-opus", "claude-3-sonnet", "claude-3-haiku", "claude-3-5-"})) {
        ModelThinkingCapability capability;
        capability.can_disable_thinking = true;
        capability.supported_efforts.clear();
        capability.supports_thinking_budget = false;
        capability.supports_sampling_params = true;
        return capability;
    }
    // Checked before the dated 4.x markers so a dated 4.6 id cannot fall into the budget list.
    if (model.find("4-6") != std::string::npos || model.find("4.6") != std::string::npos) {
        return derive(anthropic, CapabilitySpec{.supports_sampling_params = true});
    }
    const auto any_contains = [&](std::initializer_list<std::string_view> markers) {
        for (std::string_view marker : markers) {
            if (model.find(marker) != std::string::npos) return true;
        }
        return false;
    };
    if (any_contains({"claude-3-7", "-4-0", "-4-1", "-4-5", "4.0", "4.1", "4.5", "-4-2025"})) {
        ModelThinkingCapability capability;
        capability.can_disable_thinking = true;
        capability.supported_efforts.clear();
        capability.supports_thinking_budget = true;
        capability.min_budget_tokens = 1024;
        capability.max_budget_tokens.reset();
        capability.supports_effort_with_budget = false;
        capability.supports_sampling_params = true;
        return capability;
    }
    return anthropic_undocumented;
}

/** qwenCapability: effort group, glm, DashScope DeepSeek/Kimi, thinking-only and hybrid series. */
ModelThinkingCapability qwen_capability(const std::string& model) {
    if (model == "qwen3.8-max" || starts_with(model, "qwen3.8-max-") ||
        model == "qwen3.8-flash" || starts_with(model, "qwen3.8-flash-") ||
        model == "qwen3.8-27b" || model == "qwen3.8-2.4t-a95b") {
        return make_qwen38_effort();
    }
    if (starts_with(model, "glm-5.3") || starts_with(model, "zhipu/glm-5.3")) {
        ModelThinkingCapability capability;
        capability.can_disable_thinking = false;
        capability.supported_efforts = {"low", "high", "max"};
        capability.supports_thinking_budget = false;
        return capability;
    }
    if (starts_with(model, "deepseek-v4") || starts_with(model, "kimi/kimi-k3") ||
        model == "kimi-k3") {
        return derive(make_deep_seek(), CapabilitySpec{
                                            .supports_thinking_budget = true,
                                            .max_budget_tokens = 32768,
                                        });
    }
    for (const std::string& thinking_only : qwen_thinking_only_models()) {
        if (model == thinking_only) {
            return derive(make_qwen_hybrid(), CapabilitySpec{.can_disable_thinking = false});
        }
    }
    if (starts_with(model, "qwq-plus-")) {
        return derive(make_qwen_hybrid(), CapabilitySpec{.can_disable_thinking = false});
    }
    if (starts_with(model, "qwen")) return make_qwen_hybrid();
    return ModelThinkingCapability::permissive();
}

/** ollamaCapability: "bare" model names carry the levels; every other model is boolean. */
ModelThinkingCapability ollama_capability(const std::string& model) {
    const std::size_t slash = model.rfind('/');
    const std::string after_slash = slash == std::string::npos ? model : model.substr(slash + 1);
    const std::size_t colon = after_slash.find(':');
    const std::string bare = colon == std::string::npos ? after_slash : after_slash.substr(0, colon);
    if (bare == "gpt-oss") {
        ModelThinkingCapability capability = make_ollama();
        capability.supported_efforts = {"low", "medium", "high"};
        return capability;
    }
    return make_ollama();
}

/** groqCapability: GPT-OSS forced reasoning, Qwen 3.6 single value, undocumented permissive. */
ModelThinkingCapability groq_capability(const std::string& model) {
    if (starts_with(model, "openai/gpt-oss")) {
        return derive(make_groq(), CapabilitySpec{.can_disable_thinking = false});
    }
    if (model == "qwen/qwen3.6-27b") {
        return derive(make_groq(), CapabilitySpec{.supported_efforts = {std::vector<std::string>{"default"}}});
    }
    if (model == "qwen/qwen3.8-27b") return make_groq();
    return derive(ModelThinkingCapability::permissive(),
                  CapabilitySpec{.supports_thinking_budget = false});
}

// -- hooks helpers ------------------------------------------------------------------------------

bool call_id_is_wire_safe(const std::string& value);
bool call_name_is_wire_safe(const std::string& value);

bool contains_duplicate(const std::vector<std::string>& seen, const std::string& value) {
    return std::find(seen.begin(), seen.end(), value) != seen.end();
}

// -- registry -----------------------------------------------------------------------------------

std::string random_uuid_v4() {
    static thread_local std::mt19937_64 engine([] {
        std::random_device device;
        const std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        std::seed_seq seq{static_cast<std::uint32_t>(seed),
                          static_cast<std::uint32_t>(seed >> 32)};
        return std::mt19937_64{seq};
    }());
    std::uint64_t first = engine();
    std::uint64_t second = engine();
    std::uint8_t bytes[16];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>((first >> (8 * (7 - i))) & 0xFF);
    for (int i = 0; i < 8; ++i) {
        bytes[8 + i] = static_cast<std::uint8_t>((second >> (8 * (7 - i))) & 0xFF);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

std::int64_t file_mtime_nanos(const std::filesystem::path& path) {
    const auto stamp = std::filesystem::last_write_time(path);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
}

std::array<std::uint8_t, 32> hash_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw ProviderRequestError{"IOException", "Cannot open attachment"};
    Sha256Digest digest;
    char buffer[8192];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
        digest.update(reinterpret_cast<const std::uint8_t*>(buffer),
                      static_cast<std::size_t>(stream.gcount()));
        if (stream.gcount() == 0) break;
    }
    return digest.finish();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// ThinkingLevels / capability layer
// ---------------------------------------------------------------------------------------------

const std::vector<std::string>& effort_values() {
    static const std::vector<std::string> kEfforts = {"minimal", "low", "medium",
                                                      "high",    "xhigh", "max"};
    return kEfforts;
}

std::string normalize_effort_level(const std::string& raw) {
    const std::string value = lower_ascii(json::trim(raw));
    if (json::is_blank(value)) return std::string{kDefaultEffort};
    // Legacy compat: old budget mode now migrates into independent budget fields.
    if (value == "auto" || value == "budget:dynamic" || starts_with(value, "budget:")) {
        return std::string{kDefaultEffort};
    }
    return normalize_effort_alias(value);
}

std::string effort_for_index(std::size_t index) {
    const auto& efforts = effort_values();
    return index < efforts.size() ? efforts[index] : std::string{kDefaultEffort};
}

std::size_t index_for_effort(const std::string& effort) {
    // indexOf(normalizeEffort(effort)).coerceAtLeast(0): a level outside the ordered list lands
    // on the lowest position, matching the Kotlin helper's UI-facing clamp.
    const std::string normalized = normalize_effort_alias(effort);
    const auto& order = effort_values();
    const auto it = std::find(order.begin(), order.end(), normalized);
    return it == order.end() ? 0 : static_cast<std::size_t>(it - order.begin());
}

std::optional<std::int32_t> legacy_budget_tokens(const std::string& raw) {
    const std::string value = lower_ascii(json::trim(raw));
    if (!starts_with(value, "budget:")) return std::nullopt;
    std::string_view digits(value.data() + 7, value.size() - 7);
    // Kotlin String.toIntOrNull also accepts a leading '+' sign.
    if (!digits.empty() && digits.front() == '+') digits.remove_prefix(1);
    if (digits.empty()) return std::nullopt;
    std::int32_t parsed = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    // takeIf { it > 0 }
    return parsed > 0 ? std::optional<std::int32_t>{parsed} : std::nullopt;
}

const ModelThinkingCapability& ModelThinkingCapability::permissive() {
    static const ModelThinkingCapability kPermissive{};
    return kPermissive;
}

std::optional<std::string> ModelThinkingCapability::lowest_effort() const {
    if (supported_efforts.empty()) return std::nullopt;
    return supported_efforts.front();
}

std::optional<std::string> ModelThinkingCapability::nearest_effort(const std::string& effort) const {
    if (supported_efforts.empty()) return std::nullopt;
    const std::string normalized = normalize_effort_level(effort);
    if (std::find(supported_efforts.begin(), supported_efforts.end(), normalized) !=
        supported_efforts.end()) {
        return normalized;
    }
    const auto& order = effort_values();
    const auto wanted_it = std::find(order.begin(), order.end(), normalized);
    if (wanted_it == order.end()) return supported_efforts.front();
    const std::ptrdiff_t wanted = wanted_it - order.begin();

    const auto score = [&](const std::string& candidate) -> std::size_t {
        const auto it = std::find(order.begin(), order.end(), candidate);
        // Kotlin indexOf semantics: -1 for a value outside the ordered selector set (cannot
        // happen — every entry is drawn from effortValues — but keep the arithmetic).
        const std::ptrdiff_t index = it == order.end() ? -1 : (it - order.begin());
        const std::ptrdiff_t distance = index > wanted ? index - wanted : wanted - index;
        // Equal distances resolve upward, so a request the model does not accept lands on at
        // least the amount of thinking it asked for.
        return static_cast<std::size_t>(distance * 2 + (index < wanted ? 1 : 0));
    };

    std::optional<std::string> best;
    std::size_t best_score = 0;
    for (const std::string& candidate : supported_efforts) {
        const std::size_t value = score(candidate);
        if (!best || value < best_score) {
            best = candidate;
            best_score = value;
        }
    }
    return best;
}

std::int32_t ModelThinkingCapability::clamp_budget(std::int32_t tokens) const {
    const std::int32_t ceiling =
        max_budget_tokens.value_or(std::numeric_limits<std::int32_t>::max());
    return std::clamp(tokens, min_budget_tokens, ceiling);
}

ThinkingProviderFamily family_for(const std::optional<std::string>& provider_name,
                                  const std::optional<std::string>& custom_protocol_wire_value) {
    if (custom_protocol_wire_value) {
        std::string protocol = lower_ascii(json::trim(*custom_protocol_wire_value));
        if (!protocol.empty()) {
            if (protocol == "anthropic" || protocol == "claude") {
                return ThinkingProviderFamily::Anthropic;
            }
            if (protocol == "google" || protocol == "gemini") {
                return ThinkingProviderFamily::Gemini;
            }
            // "openai" and every unknown protocol: OpenAI-compatible wire.
            return ThinkingProviderFamily::OpenAiCompatible;
        }
    }
    const std::string name = provider_name ? lower_ascii(json::trim(*provider_name)) : "";
    if (name == "anthropic") return ThinkingProviderFamily::Anthropic;
    if (name == "openai") return ThinkingProviderFamily::OpenAi;
    if (name == "open router" || name == "openrouter") return ThinkingProviderFamily::OpenRouter;
    if (name == "deepseek") return ThinkingProviderFamily::DeepSeek;
    if (name == "qwen") return ThinkingProviderFamily::Qwen;
    if (name == "groq") return ThinkingProviderFamily::Groq;
    if (name == "google" || name == "gemini") return ThinkingProviderFamily::Gemini;
    if (name == "ollama") return ThinkingProviderFamily::Ollama;
    if (name == "local") return ThinkingProviderFamily::Local;
    return ThinkingProviderFamily::OpenAiCompatible;
}

const ModelThinkingCapability& family_default(ThinkingProviderFamily family) {
    switch (family) {
        case ThinkingProviderFamily::Anthropic:
            return *[] {
                static const ModelThinkingCapability kValue = make_anthropic();
                return &kValue;
            }();
        case ThinkingProviderFamily::OpenAi:
            return *[] {
                static const ModelThinkingCapability kValue = make_open_ai();
                return &kValue;
            }();
        case ThinkingProviderFamily::OpenAiCompatible:
            return ModelThinkingCapability::permissive();
        case ThinkingProviderFamily::OpenRouter:
            return *[] {
                static const ModelThinkingCapability kValue = make_open_router();
                return &kValue;
            }();
        case ThinkingProviderFamily::DeepSeek:
            return *[] {
                static const ModelThinkingCapability kValue = make_deep_seek();
                return &kValue;
            }();
        case ThinkingProviderFamily::Qwen:
            return *[] {
                static const ModelThinkingCapability kValue = make_qwen_hybrid();
                return &kValue;
            }();
        case ThinkingProviderFamily::Groq:
            return *[] {
                static const ModelThinkingCapability kValue = make_groq();
                return &kValue;
            }();
        case ThinkingProviderFamily::Gemini:
            return *[] {
                static const ModelThinkingCapability kValue = make_gemini();
                return &kValue;
            }();
        case ThinkingProviderFamily::Ollama:
            return *[] {
                static const ModelThinkingCapability kValue = make_ollama();
                return &kValue;
            }();
        case ThinkingProviderFamily::Local: {
            static const ModelThinkingCapability kValue = [] {
                ModelThinkingCapability capability;
                capability.supported_efforts.clear();
                capability.supports_thinking_budget = false;
                return capability;
            }();
            return kValue;
        }
    }
    return ModelThinkingCapability::permissive();
}

ModelThinkingCapability capability_for_model(ThinkingProviderFamily family,
                                             const std::string& model_id) {
    const std::string model = lower_ascii(json::trim(model_id));
    switch (family) {
        case ThinkingProviderFamily::Anthropic:
            return anthropic_capability(model);
        case ThinkingProviderFamily::OpenAi: {
            if (model == "gpt-6-astra") {
                ModelThinkingCapability capability = make_open_ai();
                capability.can_disable_thinking = false;
                capability.supported_efforts = {"low", "medium", "high", "xhigh", "max"};
                return capability;
            }
            if (model == "gpt-6-sol" || model == "gpt-6-luna") {
                ModelThinkingCapability capability = make_open_ai();
                capability.supported_efforts = {"none", "low", "medium", "high", "xhigh", "max"};
                return capability;
            }
            return make_open_ai();
        }
        case ThinkingProviderFamily::Ollama:
            return ollama_capability(model);
        case ThinkingProviderFamily::Qwen:
            return qwen_capability(model);
        case ThinkingProviderFamily::Groq:
            return groq_capability(model);
        default:
            return family_default(family);
    }
}

ResolvedThinking resolve_thinking(const ModelThinkingCapability& capability,
                                  bool requested_enabled,
                                  const std::string& requested_effort,
                                  bool requested_budget_enabled,
                                  std::int32_t requested_budget_tokens) {
    if (!requested_enabled) {
        if (capability.can_disable_thinking) {
            return ResolvedThinking{false, std::nullopt, std::nullopt, capability};
        }
        // Thinking cannot be turned off for this model: use its cheapest valid setting rather
        // than rejecting the request, and ignore the caller's unrelated effort value.
        return ResolvedThinking{true, capability.lowest_effort(), std::nullopt, capability};
    }

    const bool budget_requested =
        requested_budget_enabled && capability.supports_thinking_budget;
    const std::optional<std::int32_t> budget =
        budget_requested ? std::optional<std::int32_t>{capability.clamp_budget(
                               requested_budget_tokens)}
                         : std::nullopt;
    std::optional<std::string> effort;
    if (budget && !capability.supports_effort_with_budget) {
        effort.reset();
    } else {
        effort = capability.nearest_effort(requested_effort);
    }
    return ResolvedThinking{true, effort, budget, capability};
}

ResolvedThinking resolved_thinking(const ProviderConfig& config, ThinkingProviderFamily family) {
    return resolve_thinking(capability_for_model(family, config.model_id),
                            config.thinking_enabled, config.thinking_level,
                            config.thinking_budget_enabled, config.thinking_budget_tokens);
}

ModelThinkingCapability capability_for_provider(
    const std::optional<std::string>& provider_name, const std::string& model_id,
    const std::optional<std::string>& custom_protocol_wire_value) {
    return capability_for_model(family_for(provider_name, custom_protocol_wire_value), model_id);
}

// -- cheapest-inference helpers ------------------------------------------------------------------

bool is_cheapest_inference_endpoint(std::string_view default_base_url) {
    const std::string haystack = lower_ascii(std::string(default_base_url));
    return haystack.find("cheapestinference.com") != std::string::npos;
}

std::string cheapest_inference_effort(bool disabled, const std::optional<std::string>& effort) {
    if (disabled) return "none";
    if (effort && (*effort == "none" || *effort == "minimal" || *effort == "low")) return "low";
    if (effort && *effort == "medium") return "medium";
    return "high";
}

// ---------------------------------------------------------------------------------------------
// The six provider hooks
// ---------------------------------------------------------------------------------------------

OpenAiChatRequest with_open_ai_thinking(OpenAiChatRequest request,
                                        const ResolvedThinking& resolved) {
    std::optional<std::string> effort;
    if (resolved.disabled()) {
        if (resolved.capability.supports_effort()) effort = "none";
    } else {
        effort = resolved.effort;
    }
    if (!effort) return request;
    request.reasoning_effort = effort;
    return request;
}

OpenAiChatRequest with_deep_seek_thinking(OpenAiChatRequest request,
                                          const ResolvedThinking& resolved) {
    if (resolved.disabled()) {
        request.thinking = OpenAiThinking{"disabled"};
        return request;
    }
    request.thinking = OpenAiThinking{"enabled"};
    request.reasoning_effort = resolved.effort;
    return request;
}

OpenAiChatRequest with_groq_thinking(OpenAiChatRequest request, const ResolvedThinking& resolved,
                                     bool thinking_enabled) {
    const ModelThinkingCapability& capability = resolved.capability;
    if (!thinking_enabled && !capability.can_disable_thinking) {
        request.reasoning_effort = resolved.effort;
        request.include_reasoning = false;
        return request;
    }
    std::optional<std::string> effort;
    if (resolved.disabled()) {
        if (capability.supports_effort()) effort = "none";
    } else {
        effort = resolved.effort;
    }
    request.reasoning_effort = effort;
    return request;
}

OpenAiChatRequest with_qwen_thinking(OpenAiChatRequest request, const ResolvedThinking& resolved) {
    if (resolved.capability.supports_effort()) {
        request.reasoning_effort = resolved.disabled() ? std::optional<std::string>{"none"}
                                                        : resolved.effort;
        request.thinking_budget = resolved.budget_tokens;
    } else {
        request.enable_thinking = resolved.enabled;
        request.thinking_budget = resolved.budget_tokens;
    }
    return request;
}

OpenAiChatRequest with_open_router_reasoning(OpenAiChatRequest request,
                                             const ResolvedThinking& resolved,
                                             bool google_search_enabled) {
    OpenAiReasoning reasoning;
    if (resolved.disabled()) {
        reasoning.enabled = false;
    } else {
        reasoning.effort = resolved.effort;
        reasoning.max_tokens = resolved.budget_tokens;
    }
    request.reasoning = reasoning;
    if (google_search_enabled) {
        request.plugins = std::vector<OpenAiPlugin>{OpenAiPlugin{"web"}};
    } else {
        request.plugins.reset();
    }
    return request;
}

OpenAiChatRequest with_custom_open_ai_thinking(OpenAiChatRequest request,
                                               const ResolvedThinking& resolved,
                                               std::string_view default_base_url) {
    if (is_cheapest_inference_endpoint(default_base_url)) {
        request.reasoning_effort =
            cheapest_inference_effort(resolved.disabled(), resolved.effort);
        request.thinking_budget.reset();
        return request;
    }
    std::optional<std::string> effort;
    if (resolved.disabled()) {
        if (resolved.capability.supports_effort()) effort = "none";
    } else {
        effort = resolved.effort;
    }
    request.reasoning_effort = effort;
    request.thinking_budget = resolved.budget_tokens;
    return request;
}

// ---------------------------------------------------------------------------------------------
// buildToolCallId
// ---------------------------------------------------------------------------------------------

namespace {

/** Decodes UTF-8 into UTF-16 code units; invalid bytes decode to '?' like the JVM UTF-8 decoder. */
std::vector<char16_t> utf16_units(std::string_view text) {
    std::vector<char16_t> units;
    units.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            units.push_back(static_cast<char16_t>(lead));
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code_point = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code_point = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code_point = lead & 0x07;
        }
        if (length == 0 || index + length > text.size()) {
            units.push_back(u'?');
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        if (!valid || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            units.push_back(u'?');
            ++index;
            continue;
        }
        if (code_point <= 0xFFFF) {
            units.push_back(static_cast<char16_t>(code_point));
        } else {
            code_point -= 0x10000;
            units.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
            units.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
        }
        index += length;
    }
    return units;
}

bool safe_id_unit(char16_t unit) {
    return (unit >= u'0' && unit <= u'9') || (unit >= u'A' && unit <= u'Z') ||
           (unit >= u'a' && unit <= u'z') || unit == u'_' || unit == u'-';
}

}  // namespace

std::string build_tool_call_id(std::string_view tool_name, std::string_view arguments,
                               std::string_view prefix) {
    const std::string input = std::string(tool_name) + ":" + std::string(arguments);
    const auto digest = sha256(input);
    const std::string short_hash = to_hex_lower(digest.data(), 8);

    // .trim().replace(Regex("[^A-Za-z0-9_-]+"), "_").trim('_').take(32).ifBlank { "tool" }
    const std::string trimmed = json::trim(tool_name);
    const std::vector<char16_t> units = utf16_units(trimmed);
    std::string safe_name;
    safe_name.reserve(units.size());
    bool in_run = false;
    for (char16_t unit : units) {
        if (safe_id_unit(unit)) {
            if (in_run) {
                safe_name.push_back('_');
                in_run = false;
            }
            if (unit < 0x80) {
                safe_name.push_back(static_cast<char>(unit));
            } else {
                // Safe units are ASCII by construction; keep a defensive replacement.
                safe_name.push_back('?');
            }
        } else {
            in_run = true;
        }
    }
    if (in_run) safe_name.push_back('_');

    // .trim('_')
    std::size_t edge_begin = 0;
    std::size_t edge_end = safe_name.size();
    while (edge_begin < edge_end && safe_name[edge_begin] == '_') ++edge_begin;
    while (edge_end > edge_begin && safe_name[edge_end - 1] == '_') --edge_end;
    const std::string without_edges = safe_name.substr(edge_begin, edge_end - edge_begin);

    const std::string taken = json::take_utf16_units(without_edges, 32);
    const std::string name = json::is_blank(taken) ? "tool" : taken;
    std::string out(prefix);
    out += name;
    out.push_back('_');
    out += short_hash;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Message conversion
// ---------------------------------------------------------------------------------------------

std::string image_mime_type(std::string_view image_path) {
    const auto ends_with_ignore_case = [](std::string_view text, std::string_view suffix) {
        if (text.size() < suffix.size()) return false;
        return lower_ascii(std::string(text.substr(text.size() - suffix.size()))) == suffix;
    };
    if (ends_with_ignore_case(image_path, ".png")) return "image/png";
    if (ends_with_ignore_case(image_path, ".webp")) return "image/webp";
    if (ends_with_ignore_case(image_path, ".gif")) return "image/gif";
    return "image/jpeg";
}

namespace {

std::optional<std::string> join_thought_content(const std::optional<std::vector<MessageSegment>>& segments) {
    if (!segments) return std::nullopt;
    std::string joined;
    bool first = true;
    for (const MessageSegment& segment : *segments) {
        if (segment.type != "thought") continue;
        if (!first) joined.push_back('\n');
        first = false;
        joined += segment.content;
    }
    return joined;
}

const MessageSegment* first_segment_with_items(const std::vector<MessageSegment>& segments) {
    for (const MessageSegment& segment : segments) {
        if (!segment.response_output_items.empty()) return &segment;
    }
    return nullptr;
}

std::vector<MessageSegment> tool_segments(const std::optional<std::vector<MessageSegment>>& segments) {
    std::vector<MessageSegment> result;
    if (!segments) return result;
    for (const MessageSegment& segment : *segments) {
        if (segment.type == "tool") result.push_back(segment);
    }
    return result;
}

}  // namespace

std::vector<OpenAiMessage> convert_to_open_ai_messages(
    const std::vector<ChatMessage>& messages,
    const std::optional<std::string>& system_prompt,
    bool include_images,
    Base64FileRegistry& base64_files,
    bool forward_assistant_reasoning) {
    std::vector<OpenAiMessage> api_messages;

    if (system_prompt && !json::is_blank(*system_prompt)) {
        api_messages.push_back(OpenAiMessage{
            "system",
            std::vector<OpenAiContentPart>{OpenAiContentPart{"text", *system_prompt, std::nullopt}},
            std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    }

    for (const ChatMessage& msg : messages) {
        // -- tool_ messages: assistant turn with tool_calls only ---------------------------------
        if (starts_with(msg.id, kToolMsgPrefix)) {
            const std::optional<std::string> thought_content = join_thought_content(msg.segments);
            const std::optional<std::string> reasoning_content =
                thought_content && !thought_content->empty()
                    ? std::optional<std::string>{*thought_content}
                    : std::nullopt;

            const std::vector<MessageSegment> segs = tool_segments(msg.segments);
            if (!segs.empty()) {
                std::vector<OpenAiRequestToolCall> tool_calls;
                tool_calls.reserve(segs.size());
                for (const MessageSegment& seg : segs) {
                    const std::string tool_name = seg.tool_name.value_or("");
                    const std::string tool_args = seg.tool_args.value_or("{}");
                    OpenAiRequestToolCall call;
                    call.id = seg.tool_call_id ? *seg.tool_call_id
                                               : build_tool_call_id(tool_name, tool_args);
                    call.function.name = tool_name;
                    call.function.arguments = tool_args;
                    tool_calls.push_back(std::move(call));
                }
                const MessageSegment* with_items = first_segment_with_items(segs);
                OpenAiMessage message;
                message.role = "assistant";
                message.content = std::nullopt;
                message.tool_calls = std::move(tool_calls);
                message.reasoning_content = reasoning_content;
                if (with_items) {
                    message.response_output_items = with_items->response_output_items;
                    message.response_output_item_provider = with_items->response_output_item_provider;
                }
                message.response_output_item_model = msg.model_name;
                api_messages.push_back(std::move(message));
            } else if (msg.tool_call) {
                const ToolCallData& tc = *msg.tool_call;
                OpenAiMessage message;
                message.role = "assistant";
                message.content = std::nullopt;
                OpenAiRequestToolCall call;
                call.id = tc.tool_call_id ? *tc.tool_call_id
                                          : build_tool_call_id(tc.tool_name, tc.arguments);
                call.function.name = tc.tool_name;
                call.function.arguments = tc.arguments;
                message.tool_calls = std::vector<OpenAiRequestToolCall>{std::move(call)};
                message.reasoning_content = reasoning_content;
                if (!tc.response_output_items.empty()) {
                    message.response_output_items = tc.response_output_items;
                }
                message.response_output_item_provider = tc.response_output_item_provider;
                message.response_output_item_model = msg.model_name;
                api_messages.push_back(std::move(message));
            }
            continue;
        }

        // -- result_ messages carry the tool result(s) --------------------------------------------
        if (starts_with(msg.id, kResultMsgPrefix)) {
            const std::vector<MessageSegment> segs = tool_segments(msg.segments);
            if (!segs.empty()) {
                for (const MessageSegment& seg : segs) {
                    const std::string tool_name = seg.tool_name.value_or("");
                    const std::string tool_args = seg.tool_args.value_or("{}");
                    OpenAiMessage message;
                    message.role = "tool";
                    message.content = std::vector<OpenAiContentPart>{
                        OpenAiContentPart{"text", seg.tool_result.value_or(""), std::nullopt}};
                    message.tool_call_id = seg.tool_call_id
                                               ? *seg.tool_call_id
                                               : build_tool_call_id(tool_name, tool_args);
                    api_messages.push_back(std::move(message));
                }
            } else if (msg.tool_call) {
                const ToolCallData& tc = *msg.tool_call;
                OpenAiMessage message;
                message.role = "tool";
                message.content = std::vector<OpenAiContentPart>{
                    OpenAiContentPart{"text", tc.result, std::nullopt}};
                message.tool_call_id =
                    tc.tool_call_id ? *tc.tool_call_id
                                    : build_tool_call_id(tc.tool_name, tc.arguments);
                api_messages.push_back(std::move(message));
            }
            continue;
        }

        // -- Normal message: text + images ---------------------------------------------------------
        std::vector<OpenAiContentPart> parts;
        if (!msg.text.empty()) {
            parts.push_back(OpenAiContentPart{"text", msg.text, std::nullopt});
        }

        if (include_images && msg.participant == Participant::User) {
            for (const std::string& image_path : msg.images) {
                const std::optional<std::string> placeholder = base64_files.register_file(image_path);
                if (!placeholder) continue;
                const std::string url =
                    "data:" + image_mime_type(image_path) + ";base64," + *placeholder;
                parts.push_back(OpenAiContentPart{
                    "image_url", std::nullopt, OpenAiImageUrl{url}});
            }
        }

        if (parts.empty()) {
            parts.push_back(OpenAiContentPart{"text", "[Attachment unavailable]", std::nullopt});
        }

        OpenAiMessage message;
        message.role = msg.participant == Participant::User ? "user" : "assistant";
        message.content = std::move(parts);
        if (forward_assistant_reasoning && msg.participant != Participant::User) {
            const std::optional<std::string> joined = join_thought_content(msg.segments);
            if (joined && !json::is_blank(*joined)) message.reasoning_content = *joined;
        }
        api_messages.push_back(std::move(message));
    }

    return api_messages;
}

// ---------------------------------------------------------------------------------------------
// Base64 file registry
// ---------------------------------------------------------------------------------------------

std::string Base64FilePart::diagnostic_marker() const {
    return "[STREAMED_BASE64:" + std::to_string(byte_count) + "_BYTES]";
}

bool Base64FilePart::metadata_matches() const {
    std::error_code error;
    const std::filesystem::path path(this->path);
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return false;
    if (static_cast<std::int64_t>(size) != byte_count) return false;
    return file_mtime_nanos(path) == last_modified;
}

std::optional<std::string> Base64FileRegistry::register_file(const std::string& path) {
    try {
        std::error_code error;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(path, error);
        if (error) return std::nullopt;
        if (!std::filesystem::is_regular_file(canonical, error) || error) return std::nullopt;
        const auto size = std::filesystem::file_size(canonical, error);
        if (error) return std::nullopt;
        const std::int64_t byte_count = static_cast<std::int64_t>(size);
        const std::int64_t last_modified = file_mtime_nanos(canonical);
        const auto digest = hash_file(canonical);
        // Re-read the metadata after hashing: a file replaced mid-hash must not be projected.
        if (!std::filesystem::is_regular_file(canonical, error) || error) return std::nullopt;
        if (std::filesystem::file_size(canonical, error) != size || error) return std::nullopt;
        if (file_mtime_nanos(canonical) != last_modified) return std::nullopt;

        Base64FilePart part;
        part.placeholder = "__AGORA_BASE64_" + random_uuid_v4() + "__";
        part.path = canonical.string();
        part.byte_count = byte_count;
        part.last_modified = last_modified;
        part.sha256 = digest;
        files_.push_back(std::move(part));
        return files_.back().placeholder;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<StreamingJsonBody> Base64FileRegistry::prepare(
    const std::string& template_json) const {
    if (files_.empty()) return std::nullopt;

    struct Positioned {
        std::size_t position;
        const Base64FilePart* part;
    };
    std::vector<Positioned> positioned;
    positioned.reserve(files_.size());
    for (const Base64FilePart& part : files_) {
        const std::size_t position = template_json.find(part.placeholder);
        if (position == std::string::npos) {
            throw_illegal_argument("Base64 placeholder is missing from serialized request");
        }
        if (template_json.find(part.placeholder, position + part.placeholder.size()) !=
            std::string::npos) {
            throw_illegal_argument("Base64 placeholder occurs more than once in serialized request");
        }
        positioned.push_back(Positioned{position, &part});
    }
    // Kotlin sortedBy is stable: equal positions keep registration order.
    std::stable_sort(positioned.begin(), positioned.end(),
                     [](const Positioned& left, const Positioned& right) {
                         return left.position < right.position;
                     });

    StreamingJsonBody body;
    body.segments.reserve(positioned.size() + 1);
    body.parts.reserve(positioned.size());
    body.diagnostic_json.reserve(template_json.size());
    std::size_t cursor = 0;
    for (const Positioned& entry : positioned) {
        if (entry.position < cursor) throw_illegal_argument("Base64 placeholders overlap");
        const std::string segment = template_json.substr(cursor, entry.position - cursor);
        body.segments.push_back(segment);
        body.parts.push_back(*entry.part);
        body.diagnostic_json += segment;
        body.diagnostic_json += entry.part->diagnostic_marker();
        cursor = entry.position + entry.part->placeholder.size();
    }
    const std::string final_segment = template_json.substr(cursor);
    body.segments.push_back(final_segment);
    body.diagnostic_json += final_segment;

    std::int64_t total = 0;
    for (const std::string& segment : body.segments) {
        total += static_cast<std::int64_t>(segment.size());
    }
    for (const Base64FilePart& part : body.parts) {
        if (part.byte_count < 0) throw_illegal_argument("Image attachment length is invalid");
        const std::int64_t complete_groups = part.byte_count / 3;
        const std::int64_t remainder_length = part.byte_count % 3 == 0 ? 0 : 4;
        total += complete_groups * 4 + remainder_length;
    }
    body.content_length = total;
    return body;
}

namespace {

std::string read_attachment(const Base64FilePart& part, const char* failure_message) {
    std::error_code error;
    const std::filesystem::path path(part.path);
    if (!std::filesystem::is_regular_file(path, error) || error) {
        throw ProviderRequestError{"IOException", failure_message};
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || static_cast<std::int64_t>(size) != part.byte_count) {
        throw ProviderRequestError{"IOException", failure_message};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw ProviderRequestError{"IOException", failure_message};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0) stream.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!stream && size > 0) throw ProviderRequestError{"IOException", failure_message};
    return bytes;
}

}  // namespace

std::string StreamingJsonBody::resolved_bytes() const {
    std::string out = segments.front();
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const Base64FilePart& part = parts[index];
        std::error_code error;
        const std::filesystem::path path(part.path);
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw ProviderRequestError{"IOException", "Attachment is not readable"};
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        out += encode_base64(buffer.str());
        out += segments[index + 1];
    }
    return out;
}

std::string StreamingJsonBody::write_bytes() const {
    std::string out;
    out.reserve(static_cast<std::size_t>(content_length));
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const Base64FilePart& part = parts[index];
        if (!part.metadata_matches()) {
            throw ProviderRequestError{"IOException",
                                       "Image attachment changed before request upload"};
        }
        out += segments[index];
        const std::string bytes =
            read_attachment(part, "Image attachment changed during request upload");
        if (part.metadata_matches()) {
            Sha256Digest digest;
            digest.update(bytes);
            if (digest.finish() == part.sha256) {
                out += encode_base64(bytes);
                continue;
            }
        }
        throw ProviderRequestError{"IOException", "Image attachment changed during request upload"};
    }
    out += segments.back();
    return out;
}

// ---------------------------------------------------------------------------------------------
// Wire validation gates
// ---------------------------------------------------------------------------------------------

namespace {

bool matches_wire_name(const std::string& value, std::size_t max_units) {
    if (value.empty() || value.size() > max_units) return false;
    for (unsigned char byte : value) {
        const bool safe = (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= 'a' && byte <= 'z') || byte == '_' || byte == '-';
        if (!safe) return false;
    }
    return true;
}

bool call_name_is_wire_safe(const std::string& value) { return matches_wire_name(value, 64); }
bool call_id_is_wire_safe(const std::string& value) { return matches_wire_name(value, 128); }

/** Full-document acceptance of exactly one JSON object, like validationJson.parseToJsonElement. */
bool is_json_object(const std::string& raw) {
    json::ParseResult parsed = json::parse(raw);
    return parsed.value && parsed.value->kind == json::Kind::Object;
}

void append_violations(std::vector<std::string>& target, const std::vector<std::string>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

bool is_substantive_part(const OpenAiContentPart& part) {
    if (part.type == "text") return part.text && !json::is_blank(*part.text);
    if (part.type == "image_url") {
        return part.image_url && !json::is_blank(part.image_url->url);
    }
    return false;
}

void validate_content(const std::optional<std::vector<OpenAiContentPart>>& parts,
                      const std::string& location, std::vector<std::string>& violations) {
    if (!parts) return;
    if (parts->empty()) {
        violations.push_back(location + " content is empty");
        return;
    }
    for (std::size_t index = 0; index < parts->size(); ++index) {
        const OpenAiContentPart& part = (*parts)[index];
        const std::string part_location = location + ".content[" + std::to_string(index) + "]";
        if (part.type == "text") {
            if (!part.text || part.image_url) {
                violations.push_back(part_location + " is not a valid text part");
            }
        } else if (part.type == "image_url") {
            if (!part.image_url || json::is_blank(part.image_url->url) || part.text) {
                violations.push_back(part_location + " is not a valid image_url part");
            }
        } else {
            violations.push_back(part_location + " has unsupported type " + part.type);
        }
    }
}

std::vector<std::string> property_violations(const std::string& tool_name,
                                             const std::string& path,
                                             const ToolProperty& property) {
    std::vector<std::string> violations;
    if (json::is_blank(path)) violations.push_back("tool " + tool_name + " has a blank property name");
    if (json::is_blank(property.type)) {
        violations.push_back("tool " + tool_name + " property " + path + " has no type");
    }
    if (property.type == "array" && !property.items) {
        violations.push_back("tool " + tool_name + " array " + path + " has no items schema");
    }
    if (property.type == "object" && (!property.properties || property.properties->empty())) {
        violations.push_back("tool " + tool_name + " object " + path + " has no property schema");
    }
    std::vector<std::string> declared;
    if (property.properties) {
        for (const auto& [name, child] : *property.properties) {
            (void)child;
            declared.push_back(name);
        }
    }
    if (property.required) {
        bool any_unknown = false;
        for (const std::string& name : *property.required) {
            if (!contains_duplicate(declared, name)) {
                any_unknown = true;
                break;
            }
        }
        if (any_unknown) {
            violations.push_back("tool " + tool_name + " object " + path +
                                 " requires undefined properties");
        }
    }
    if (property.items) {
        append_violations(violations, property_violations(tool_name, path + "[]", *property.items));
    }
    if (property.properties) {
        for (const auto& [name, child] : *property.properties) {
            append_violations(violations,
                              property_violations(tool_name, path + "." + name, child));
        }
    }
    return violations;
}

std::vector<std::string> validate_tool_definitions(
    const std::optional<std::vector<ToolDefinition>>& tools) {
    std::vector<std::string> violations;
    if (!tools || tools->empty()) return violations;
    std::vector<std::string> names;
    for (std::size_t index = 0; index < tools->size(); ++index) {
        const ToolDefinition& tool = (*tools)[index];
        const ToolFunction& function = tool.function;
        const std::string location = "tools[" + std::to_string(index) + "]";
        if (tool.type != "function") violations.push_back(location + ".type must be function");
        if (json::is_blank(function.name)) {
            violations.push_back(location + ".function.name is blank");
        } else if (!call_name_is_wire_safe(function.name)) {
            violations.push_back(location + ".function.name is not wire-safe");
        } else if (contains_duplicate(names, function.name)) {
            violations.push_back("duplicate tool name " + function.name);
        } else {
            names.push_back(function.name);
        }
        if (function.parameters.type != "object") {
            violations.push_back("tool " + function.name + " parameters must be an object");
        }
        bool unknown_required = false;
        for (const std::string& name : function.parameters.required) {
            bool known = false;
            for (const auto& [property_name, property] : function.parameters.properties) {
                (void)property;
                if (property_name == name) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                unknown_required = true;
                break;
            }
        }
        if (unknown_required) {
            violations.push_back("tool " + function.name + " requires undefined properties");
        }
        // An external (MCP) schema is sent verbatim: only the typed schemas Agora builds itself
        // are checked below the top level.
        if (!function.parameters.schema) {
            for (const auto& [property_name, property] : function.parameters.properties) {
                append_violations(violations,
                                  property_violations(function.name, property_name, property));
            }
        }
    }
    return violations;
}

std::vector<std::string> distinct(const std::vector<std::string>& values) {
    std::vector<std::string> out;
    for (const std::string& value : values) {
        if (!contains_duplicate(out, value)) out.push_back(value);
    }
    return out;
}

std::string join_violations(const std::vector<std::string>& violations) {
    std::string out;
    for (std::size_t index = 0; index < violations.size(); ++index) {
        if (index > 0) out += "; ";
        out += violations[index];
    }
    return out;
}

[[noreturn]] void throw_request_format(const std::string& provider,
                                       const std::vector<std::string>& violations) {
    throw ProviderRequestError{
        "RequestFormatException",
        provider + " request validation failed: " + join_violations(distinct(violations))};
}

}  // namespace

void require_valid_wire_format(const OpenAiChatRequest& request, const std::string& provider) {
    std::vector<std::string> violations;
    if (json::is_blank(request.model)) violations.push_back("model is blank");
    if (request.messages.empty()) violations.push_back("messages is empty");
    append_violations(violations, validate_tool_definitions(request.tools));
    if (request.max_tokens && *request.max_tokens <= 0) {
        violations.push_back("max_tokens must be positive");
    }
    if (request.top_p && !(*request.top_p >= 0.0f && *request.top_p <= 1.0f)) {
        violations.push_back("top_p is outside 0..1");
    }

    std::vector<std::string> pending_tool_ids;
    std::vector<std::string> seen_tool_ids;
    bool saw_non_system = false;
    const auto pending_contains = [&](const std::string& value) {
        return contains_duplicate(pending_tool_ids, value);
    };

    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const OpenAiMessage& message = request.messages[index];
        const std::string location = "messages[" + std::to_string(index) + "]";
        const bool role_ok = message.role == "system" || message.role == "user" ||
                             message.role == "assistant" || message.role == "tool";
        if (!role_ok) violations.push_back(location + " has invalid role " + message.role);
        validate_content(message.content, location, violations);

        if (message.role == "system") {
            if (saw_non_system) {
                violations.push_back(location + " system role is not at the beginning");
            }
            if (!message.content || message.content->empty() ||
                std::none_of(message.content->begin(), message.content->end(),
                             is_substantive_part)) {
                violations.push_back(location + " system content is empty");
            }
            if ((message.tool_calls && !message.tool_calls->empty()) || message.tool_call_id) {
                violations.push_back(location + " system message carries tool fields");
            }
        } else if (message.role == "user") {
            saw_non_system = true;
            if (!message.content || message.content->empty() ||
                std::none_of(message.content->begin(), message.content->end(),
                             is_substantive_part)) {
                violations.push_back(location + " user content is empty");
            }
            if (!pending_tool_ids.empty()) {
                violations.push_back(location + " interrupts pending tool results");
            }
            if ((message.tool_calls && !message.tool_calls->empty()) || message.tool_call_id) {
                violations.push_back(location + " user message carries tool fields");
            }
        } else if (message.role == "assistant") {
            saw_non_system = true;
            if (!pending_tool_ids.empty()) {
                violations.push_back(location + " starts before prior tool results are complete");
                pending_tool_ids.clear();
            }
            if (message.tool_call_id) {
                violations.push_back(location + " assistant message has tool_call_id");
            }
            const bool has_calls = message.tool_calls && !message.tool_calls->empty();
            if (!has_calls &&
                (!message.content || message.content->empty() ||
                 std::none_of(message.content->begin(), message.content->end(),
                              is_substantive_part))) {
                violations.push_back(location + " assistant content is empty");
            }
            if (message.tool_calls) {
                for (std::size_t call_index = 0; call_index < message.tool_calls->size();
                     ++call_index) {
                    const OpenAiRequestToolCall& call = (*message.tool_calls)[call_index];
                    const std::string call_location =
                        location + ".tool_calls[" + std::to_string(call_index) + "]";
                    if (!call_id_is_wire_safe(call.id)) {
                        violations.push_back(call_location + " id is not wire-safe");
                    }
                    if (call.type != "function") {
                        violations.push_back(call_location + " type is not function");
                    }
                    if (!call_name_is_wire_safe(call.function.name)) {
                        violations.push_back(call_location + " name is not wire-safe");
                    }
                    if (!is_json_object(call.function.arguments)) {
                        violations.push_back(call_location + " arguments are not a JSON object");
                    }
                    if (!json::is_blank(call.id)) {
                        if (contains_duplicate(seen_tool_ids, call.id)) {
                            violations.push_back(call_location + " reuses tool call id " + call.id);
                        } else {
                            seen_tool_ids.push_back(call.id);
                        }
                        if (pending_contains(call.id)) {
                            violations.push_back(call_location +
                                                 " duplicates tool call id " + call.id);
                        } else {
                            pending_tool_ids.push_back(call.id);
                        }
                    }
                }
            }
            if (has_calls && message.content) {
                const bool has_visible_content =
                    std::any_of(message.content->begin(), message.content->end(),
                                is_substantive_part);
                if (!has_visible_content) {
                    violations.push_back(location +
                                         " tool-call content must be null or substantive");
                }
            }
        } else if (message.role == "tool") {
            saw_non_system = true;
            if (!message.content || message.content->empty()) {
                violations.push_back(location + " tool content is absent");
            }
            const bool has_id = message.tool_call_id && !json::is_blank(*message.tool_call_id);
            if (!has_id) {
                violations.push_back(location + " tool_call_id is blank");
            } else {
                const auto it = std::find(pending_tool_ids.begin(), pending_tool_ids.end(),
                                          *message.tool_call_id);
                if (it == pending_tool_ids.end()) {
                    violations.push_back(location + " does not match a pending tool call");
                } else {
                    pending_tool_ids.erase(it);
                }
            }
            if (message.tool_calls && !message.tool_calls->empty()) {
                violations.push_back(location + " tool message carries tool_calls");
            }
        }
    }
    if (!pending_tool_ids.empty()) violations.push_back("tool calls are missing results");
    const bool ends_ok = !request.messages.empty() &&
                         (request.messages.back().role == "user" ||
                          request.messages.back().role == "tool");
    if (!ends_ok) violations.push_back("history does not end in user/tool input");
    // requireValidRequestFormat: the gate only fails when violations exist.
    if (!violations.empty()) throw_request_format(provider, violations);
}

void require_valid_serialized_request(const std::string& provider, std::string_view body,
                                      const std::vector<std::string>& required_string_fields,
                                      const std::vector<std::string>& required_array_fields) {
    const json::ParseResult parsed = json::parse(body);
    const json::Value* root =
        parsed.value && parsed.value->kind == json::Kind::Object ? &*parsed.value : nullptr;
    std::vector<std::string> violations;
    if (root == nullptr) {
        violations.push_back("serialized request is not a JSON object");
    } else {
        std::vector<std::string> seen_strings;
        for (const std::string& field : required_string_fields) {
            if (contains_duplicate(seen_strings, field)) continue;
            seen_strings.push_back(field);
            const json::Value* value = nullptr;
            for (const json::Member& entry : root->members) {
                if (entry.key == field) value = &entry.value;
            }
            std::optional<std::string> content;
            if (value != nullptr &&
                (value->kind == json::Kind::String || value->kind == json::Kind::Literal)) {
                content = value->text;  // JsonPrimitive.content: decoded text or the raw literal
            }
            if (!content || json::is_blank(*content)) {
                violations.push_back("serialized " + field + " is absent or blank");
            }
        }
        std::vector<std::string> seen_arrays;
        for (const std::string& field : required_array_fields) {
            if (contains_duplicate(seen_arrays, field)) continue;
            seen_arrays.push_back(field);
            const json::Value* value = nullptr;
            for (const json::Member& entry : root->members) {
                if (entry.key == field) value = &entry.value;
            }
            if (value == nullptr || value->kind != json::Kind::Array || value->elements.empty()) {
                violations.push_back("serialized " + field + " is absent or empty");
            }
        }
    }
    // requireValidRequestFormat: the gate only fails when violations exist.
    if (!violations.empty()) throw_request_format(provider, violations);
}

// ---------------------------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------------------------

namespace {

bool forwards_reasoning(ChatProviderKind kind, const ProviderConfig& config) {
    switch (kind) {
        case ChatProviderKind::DeepSeek:
        case ChatProviderKind::Custom:
            return config.include_assistant_reasoning;
        default:
            return false;
    }
}

OpenAiChatRequest apply_customize_hook(ChatProviderKind kind, OpenAiChatRequest request,
                                       const ProviderConfig& config) {
    switch (kind) {
        case ChatProviderKind::OpenAi:
            return with_open_ai_thinking(std::move(request),
                                         resolved_thinking(config, ThinkingProviderFamily::OpenAi));
        case ChatProviderKind::DeepSeek:
            return with_deep_seek_thinking(std::move(request),
                                           resolved_thinking(config, ThinkingProviderFamily::DeepSeek));
        case ChatProviderKind::Groq:
            return with_groq_thinking(std::move(request),
                                      resolved_thinking(config, ThinkingProviderFamily::Groq),
                                      config.thinking_enabled);
        case ChatProviderKind::Qwen:
            return with_qwen_thinking(std::move(request),
                                      resolved_thinking(config, ThinkingProviderFamily::Qwen));
        case ChatProviderKind::OpenRouter:
            return with_open_router_reasoning(
                std::move(request),
                resolved_thinking(config, ThinkingProviderFamily::OpenRouter),
                config.google_search_enabled);
        case ChatProviderKind::Custom:
            return with_custom_open_ai_thinking(
                std::move(request),
                resolved_thinking(config, ThinkingProviderFamily::OpenAiCompatible),
                config.base_url.value_or(std::string{kDefaultCustomBaseUrl}));
    }
    return request;
}

}  // namespace

BuiltChatRequest build_chat_request(ChatProviderKind kind, const std::string& provider_name,
                                     const ProviderConfig& config,
                                     const ProviderRequestInput& input) {
    Base64FileRegistry base64_files;
    const std::vector<OpenAiMessage> api_messages = convert_to_open_ai_messages(
        input.messages, input.system_prompt, config.include_images, base64_files,
        forwards_reasoning(kind, config));

    OpenAiChatRequest request;
    request.model = config.model_id;
    request.messages = api_messages;
    request.stream = true;
    request.stream_options = OpenAiStreamOptions{true};
    request.service_tier = config.open_ai_service_tier;
    request.tools = config.tools;
    request.temperature = config.temperature;
    request.max_tokens = config.max_tokens;
    request.top_p = config.top_p;
    request.frequency_penalty = config.frequency_penalty;
    request.presence_penalty = config.presence_penalty;
    request.prompt_cache_key = config.prompt_cache_key;

    request = apply_customize_hook(kind, std::move(request), config);
    require_valid_wire_format(request, provider_name);
    const std::string body_json = write_open_ai_chat_request(request);
    require_valid_serialized_request(provider_name, body_json, {"model"}, {"messages"});

    BuiltChatRequest built;
    built.api_messages = std::move(api_messages);
    built.body_json = body_json;
    built.streaming_body = base64_files.prepare(body_json);
    return built;
}

}  // namespace agora::core::openai
