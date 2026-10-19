#include "agora/core/thinking_capability.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/unicode.hpp"

namespace agora::core::thinking_capability {
namespace {

std::string trim_kotlin(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const unicode::Decoded decoded =
            unicode::decode_utf8(text.data() + begin, text.size() - begin);
        if (!unicode::is_kotlin_whitespace(decoded.code_point)) break;
        begin += decoded.length;
    }
    std::size_t end = text.size();
    while (end > begin) {
        std::size_t start = end - 1;
        while (start > begin && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
            --start;
        }
        const unicode::Decoded decoded =
            unicode::decode_utf8(text.data() + start, end - start);
        if (!unicode::is_kotlin_whitespace(decoded.code_point)) break;
        end = start;
    }
    return std::string(text.substr(begin, end - begin));
}

bool is_blank(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unicode::Decoded decoded =
            unicode::decode_utf8(text.data() + offset, text.size() - offset);
        if (!unicode::is_kotlin_whitespace(decoded.code_point)) return false;
        offset += decoded.length;
    }
    return true;
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

/** Kotlin String.toIntOrNull: optional sign, decimal digits, Int range. */
std::optional<int> parse_int(std::string_view text) {
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        negative = text[index] == '-';
        ++index;
    }
    if (index == text.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
        if (value > static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1) {
            return std::nullopt;
        }
    }
    if (negative) value = -value;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::string normalize_effort_value(std::string_view raw) {
    const std::string value = ascii_lower(trim_kotlin(raw));
    if (value == "none") return "none";
    if (value == "minimal" || value == "min") return "minimal";
    if (value == "low" || value == "quick" || value == "fast") return "low";
    if (value == "medium" || value == "balanced" || value == "balance") return "medium";
    if (value == "high" || value == "deep") return "high";
    if (value == "xhigh" || value == "x-high" || value == "extra-high" ||
        value == "extreme") {
        return "xhigh";
    }
    if (value == "max" || value == "maximum") return "max";
    return "medium";
}

int index_in_effort_values(std::string_view effort) {
    const std::vector<std::string>& values = effort_values();
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index] == effort) return static_cast<int>(index);
    }
    return -1;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

Capability make_capability(bool can_disable, std::vector<std::string> efforts,
                           bool supports_budget, int min_budget,
                           std::optional<int> max_budget, bool effort_with_budget,
                           bool sampling) {
    Capability capability;
    capability.can_disable_thinking = can_disable;
    capability.supported_efforts = std::move(efforts);
    capability.supports_thinking_budget = supports_budget;
    capability.min_budget_tokens = min_budget;
    capability.max_budget_tokens = max_budget;
    capability.supports_effort_with_budget = effort_with_budget;
    capability.supports_sampling_params = sampling;
    return capability;
}

Capability anthropic_capability() {
    return make_capability(true, {"low", "medium", "high", "xhigh", "max"}, true, 1024,
                           std::nullopt, false, false);
}

Capability anthropic_undocumented() {
    Capability capability;
    capability.min_budget_tokens = 1024;
    return capability;
}

Capability openai_capability() {
    Capability capability;
    capability.supports_thinking_budget = false;
    return capability;
}

Capability gpt6_astra() {
    Capability capability = openai_capability();
    capability.can_disable_thinking = false;
    capability.supported_efforts = {"low", "medium", "high", "xhigh", "max"};
    return capability;
}

Capability gpt6_sol_luna() {
    Capability capability = openai_capability();
    capability.supported_efforts = {"none", "low", "medium", "high", "xhigh", "max"};
    return capability;
}

Capability openrouter_capability() {
    Capability capability;
    capability.supports_effort_with_budget = false;
    return capability;
}

Capability deepseek_capability() {
    Capability capability;
    capability.supported_efforts = {"low", "high", "max"};
    capability.supports_thinking_budget = false;
    return capability;
}

Capability qwen_hybrid() {
    Capability capability;
    capability.supported_efforts = {};
    capability.max_budget_tokens = 32768;
    return capability;
}

Capability qwen38_effort() {
    Capability capability;
    capability.supported_efforts = {"low", "medium", "xhigh"};
    capability.max_budget_tokens = 32768;
    capability.supports_effort_with_budget = false;
    return capability;
}

Capability groq_capability() {
    Capability capability;
    capability.supported_efforts = {"low", "medium", "high"};
    capability.supports_thinking_budget = false;
    return capability;
}

Capability gemini_capability() {
    Capability capability;
    capability.supported_efforts = {"minimal", "low", "medium", "high"};
    capability.supports_effort_with_budget = false;
    return capability;
}

Capability ollama_capability() {
    Capability capability;
    capability.supported_efforts = {};
    capability.supports_thinking_budget = false;
    return capability;
}

Capability ollama_levels() {
    Capability capability = ollama_capability();
    capability.supported_efforts = {"low", "medium", "high"};
    return capability;
}

Capability local_capability() {
    Capability capability;
    capability.supported_efforts = {};
    capability.supports_thinking_budget = false;
    return capability;
}

Capability anthropic_model(const std::string& model) {
    if (model.rfind("claude", 0) != 0) return anthropic_undocumented();
    const std::vector<std::string_view> legacy = {
        "claude-3-opus", "claude-3-sonnet", "claude-3-haiku", "claude-3-5-",
    };
    for (std::string_view prefix : legacy) {
        if (model.rfind(prefix, 0) == 0) {
            return make_capability(true, {}, false, 1, std::nullopt, true, true);
        }
    }
    if (contains(model, "4-6") || contains(model, "4.6")) {
        Capability capability = anthropic_capability();
        capability.supports_sampling_params = true;
        return capability;
    }
    const std::vector<std::string_view> budget_only = {
        "claude-3-7", "-4-0", "-4-1", "-4-5", "4.0", "4.1", "4.5", "-4-2025",
    };
    for (std::string_view marker : budget_only) {
        if (contains(model, marker)) {
            return make_capability(true, {}, true, 1024, std::nullopt, false, true);
        }
    }
    return anthropic_undocumented();
}

bool is_qwen38_effort(const std::string& model) {
    return model == "qwen3.8-max" || model.rfind("qwen3.8-max-", 0) == 0 ||
           model == "qwen3.8-flash" || model.rfind("qwen3.8-flash-", 0) == 0 ||
           model == "qwen3.8-27b" || model == "qwen3.8-2.4t-a95b";
}

Capability qwen_model(const std::string& model) {
    if (is_qwen38_effort(model)) return qwen38_effort();

    if (model.rfind("glm-5.3", 0) == 0 || model.rfind("zhipu/glm-5.3", 0) == 0) {
        return make_capability(false, {"low", "high", "max"}, false, 1, std::nullopt, true, true);
    }

    if (model.rfind("deepseek-v4", 0) == 0 || model.rfind("kimi/kimi-k3", 0) == 0 ||
        model == "kimi-k3") {
        Capability capability = deepseek_capability();
        capability.supports_thinking_budget = true;
        capability.max_budget_tokens = 32768;
        return capability;
    }

    const std::vector<std::string_view> thinking_only = {
        "qwen3.7-max-preview", "qwen3.7-max-2026-05-17", "qwen3-next-80b-a3b-thinking",
        "qwen3-235b-a22b-thinking-2507", "qwen3-30b-a3b-thinking-2507", "qwq-plus",
    };
    bool thinking_only_model = model.rfind("qwq-plus-", 0) == 0;
    for (std::string_view id : thinking_only) {
        if (model == id) thinking_only_model = true;
    }
    if (thinking_only_model) {
        Capability capability = qwen_hybrid();
        capability.can_disable_thinking = false;
        return capability;
    }

    if (model.rfind("qwen", 0) == 0) return qwen_hybrid();
    return permissive();
}

Capability ollama_model(const std::string& model) {
    std::string bare = model.substr(model.find_last_of('/') + 1);
    const std::size_t colon = bare.find(':');
    if (colon != std::string::npos) bare = bare.substr(0, colon);
    return bare == "gpt-oss" ? ollama_levels() : ollama_capability();
}

Capability groq_model(const std::string& model) {
    if (model.rfind("openai/gpt-oss", 0) == 0) {
        Capability capability = groq_capability();
        capability.can_disable_thinking = false;
        return capability;
    }
    if (model == "qwen/qwen3.6-27b") {
        Capability capability = groq_capability();
        capability.supported_efforts = {"default"};
        return capability;
    }
    if (model == "qwen/qwen3.8-27b") return groq_capability();
    Capability capability = permissive();
    capability.supports_thinking_budget = false;
    return capability;
}

const char* bool_text(bool value) { return value ? "true" : "false"; }

std::string optional_text(const std::optional<std::string>& value) {
    return value.has_value() ? *value : "null";
}

std::string optional_int(const std::optional<int>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

std::string join_values(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) out += ", ";
        out += values[index];
    }
    return out;
}

}  // namespace

const std::vector<std::string>& effort_values() {
    static const std::vector<std::string> kValues = {
        "minimal", "low", "medium", "high", "xhigh", "max",
    };
    return kValues;
}

const std::vector<int>& budget_presets() {
    static const std::vector<int> kValues = {1024, 2048, 4096, 8192, 16384, 32768};
    return kValues;
}

std::optional<std::string> Capability::lowest_effort() const {
    if (supported_efforts.empty()) return std::nullopt;
    return supported_efforts.front();
}

Capability permissive() { return Capability{}; }

std::string normalize(const std::optional<std::string>& raw) {
    const std::string value = ascii_lower(trim_kotlin(raw.value_or(std::string())));
    if (is_blank(value)) return std::string(kDefaultEffort);
    if (value == "auto" || value == "budget:dynamic" || value.rfind("budget:", 0) == 0) {
        return std::string(kDefaultEffort);
    }
    return normalize_effort_value(value);
}

std::optional<int> legacy_budget_tokens(const std::optional<std::string>& raw) {
    const std::string value = ascii_lower(trim_kotlin(raw.value_or(std::string())));
    constexpr std::string_view prefix = "budget:";
    if (value.rfind(prefix, 0) != 0) return std::nullopt;
    const std::optional<int> parsed =
        parse_int(std::string_view(value).substr(prefix.size()));
    if (!parsed.has_value() || *parsed <= 0) return std::nullopt;
    return parsed;
}

std::string effort_for_index(int index) {
    const std::vector<std::string>& values = effort_values();
    if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        return std::string(kDefaultEffort);
    }
    return values[static_cast<std::size_t>(index)];
}

int index_for_effort(std::string_view effort) {
    const int index = index_in_effort_values(normalize_effort_value(effort));
    return index < 0 ? 0 : index;
}

ThinkingProviderFamily family_for(const std::optional<std::string>& provider_name,
                                  const std::optional<std::string>& custom_protocol) {
    if (custom_protocol.has_value()) {
        const std::string protocol = ascii_lower(trim_kotlin(*custom_protocol));
        if (!protocol.empty()) {
            if (protocol == "anthropic" || protocol == "claude") {
                return ThinkingProviderFamily::Anthropic;
            }
            if (protocol == "google" || protocol == "gemini") {
                return ThinkingProviderFamily::Gemini;
            }
            return ThinkingProviderFamily::OpenAICompatible;
        }
    }
    if (provider_name.has_value()) {
        const std::string name = ascii_lower(trim_kotlin(*provider_name));
        if (name == "anthropic") return ThinkingProviderFamily::Anthropic;
        if (name == "openai") return ThinkingProviderFamily::OpenAI;
        if (name == "open router" || name == "openrouter") {
            return ThinkingProviderFamily::OpenRouter;
        }
        if (name == "deepseek") return ThinkingProviderFamily::DeepSeek;
        if (name == "qwen") return ThinkingProviderFamily::Qwen;
        if (name == "groq") return ThinkingProviderFamily::Groq;
        if (name == "google" || name == "gemini") return ThinkingProviderFamily::Gemini;
        if (name == "ollama") return ThinkingProviderFamily::Ollama;
        if (name == "local") return ThinkingProviderFamily::Local;
    }
    return ThinkingProviderFamily::OpenAICompatible;
}

Capability family_default(ThinkingProviderFamily family) {
    switch (family) {
        case ThinkingProviderFamily::Anthropic: return anthropic_capability();
        case ThinkingProviderFamily::OpenAI: return openai_capability();
        case ThinkingProviderFamily::OpenAICompatible: return permissive();
        case ThinkingProviderFamily::OpenRouter: return openrouter_capability();
        case ThinkingProviderFamily::DeepSeek: return deepseek_capability();
        case ThinkingProviderFamily::Qwen: return qwen_hybrid();
        case ThinkingProviderFamily::Groq: return groq_capability();
        case ThinkingProviderFamily::Gemini: return gemini_capability();
        case ThinkingProviderFamily::Ollama: return ollama_capability();
        case ThinkingProviderFamily::Local: return local_capability();
    }
    return permissive();
}

Capability for_model(ThinkingProviderFamily family, std::string_view model_id) {
    const std::string model = ascii_lower(trim_kotlin(model_id));
    switch (family) {
        case ThinkingProviderFamily::Anthropic: return anthropic_model(model);
        case ThinkingProviderFamily::OpenAI:
            if (model == "gpt-6-astra") return gpt6_astra();
            if (model == "gpt-6-sol" || model == "gpt-6-luna") return gpt6_sol_luna();
            return openai_capability();
        case ThinkingProviderFamily::Ollama: return ollama_model(model);
        case ThinkingProviderFamily::Qwen: return qwen_model(model);
        case ThinkingProviderFamily::Groq: return groq_model(model);
        default: return family_default(family);
    }
}

Capability for_provider(const std::optional<std::string>& provider_name, std::string_view model_id,
                        const std::optional<std::string>& custom_protocol) {
    return for_model(family_for(provider_name, custom_protocol), model_id);
}

std::optional<std::string> nearest_effort(const Capability& capability, std::string_view effort) {
    if (capability.supported_efforts.empty()) return std::nullopt;
    const std::string normalized = normalize(std::optional<std::string>(effort));
    for (const std::string& supported : capability.supported_efforts) {
        if (supported == normalized) return normalized;
    }
    const int wanted = index_in_effort_values(normalized);
    if (wanted < 0) return capability.supported_efforts.front();

    std::size_t best = 0;
    int best_key = 0;
    bool have_best = false;
    for (std::size_t index = 0; index < capability.supported_efforts.size(); ++index) {
        const int candidate = index_in_effort_values(capability.supported_efforts[index]);
        int distance = candidate - wanted;
        if (distance < 0) distance = -distance;
        // Equal distances resolve upward: a candidate below the request pays the +1 tiebreak.
        const int key = distance * 2 + (candidate < wanted ? 1 : 0);
        if (!have_best || key < best_key) {
            have_best = true;
            best_key = key;
            best = index;
        }
    }
    return capability.supported_efforts[best];
}

int clamp_budget(const Capability& capability, int tokens) {
    const int ceiling = capability.max_budget_tokens.value_or(std::numeric_limits<int>::max());
    if (tokens < capability.min_budget_tokens) return capability.min_budget_tokens;
    if (tokens > ceiling) return ceiling;
    return tokens;
}

ResolvedThinking resolve(const Capability& capability, bool requested_enabled,
                         std::string_view requested_effort, bool requested_budget_enabled,
                         int requested_budget_tokens) {
    if (!requested_enabled) {
        if (capability.can_disable_thinking) {
            return ResolvedThinking{false, std::nullopt, std::nullopt, capability};
        }
        return ResolvedThinking{true, capability.lowest_effort(), std::nullopt, capability};
    }

    std::optional<int> budget;
    if (requested_budget_enabled && capability.supports_thinking_budget) {
        budget = clamp_budget(capability, requested_budget_tokens);
    }
    std::optional<std::string> effort;
    if (!(budget.has_value() && !capability.supports_effort_with_budget)) {
        effort = nearest_effort(capability, requested_effort);
    }
    return ResolvedThinking{true, effort, budget, capability};
}

std::string describe(ThinkingProviderFamily family) {
    switch (family) {
        case ThinkingProviderFamily::Anthropic: return "ANTHROPIC";
        case ThinkingProviderFamily::OpenAI: return "OPENAI";
        case ThinkingProviderFamily::OpenAICompatible: return "OPENAI_COMPATIBLE";
        case ThinkingProviderFamily::OpenRouter: return "OPENROUTER";
        case ThinkingProviderFamily::DeepSeek: return "DEEPSEEK";
        case ThinkingProviderFamily::Qwen: return "QWEN";
        case ThinkingProviderFamily::Groq: return "GROQ";
        case ThinkingProviderFamily::Gemini: return "GEMINI";
        case ThinkingProviderFamily::Ollama: return "OLLAMA";
        case ThinkingProviderFamily::Local: return "LOCAL";
    }
    return "OPENAI_COMPATIBLE";
}

std::string describe(const Capability& capability) {
    return "ModelThinkingCapability(canDisableThinking=" +
           std::string(bool_text(capability.can_disable_thinking)) +
           ", supportedEfforts=[" + join_values(capability.supported_efforts) +
           "], supportsThinkingBudget=" +
           std::string(bool_text(capability.supports_thinking_budget)) +
           ", minBudgetTokens=" + std::to_string(capability.min_budget_tokens) +
           ", maxBudgetTokens=" + optional_int(capability.max_budget_tokens) +
           ", supportsEffortWithBudget=" +
           std::string(bool_text(capability.supports_effort_with_budget)) +
           ", supportsSamplingParams=" +
           std::string(bool_text(capability.supports_sampling_params)) + ")";
}

std::string describe(const ResolvedThinking& resolved) {
    return "ResolvedThinking(enabled=" + std::string(bool_text(resolved.enabled)) +
           ", effort=" + optional_text(resolved.effort) +
           ", budgetTokens=" + optional_int(resolved.budget_tokens) +
           ", capability=" + describe(resolved.capability) + ")";
}

}  // namespace agora::core::thinking_capability
