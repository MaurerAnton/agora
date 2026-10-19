// Port of model/ContextBudget.kt.
//
// The only subtlety is that `normalize` migrates legacy values and clamps, while `compactLabel`
// deliberately does not migrate — live occupancy can legitimately be below 101 tokens.
#include "agora/core/context_budget.hpp"

namespace agora::core::context_budget {
namespace {

/** ContextBudget.compactUnitLabel: rounded to one decimal, dropping a trailing `.0`. */
std::string compact_unit_label(int tokens, int unit, const char* suffix) {
    const long long tenths =
        (static_cast<long long>(tokens) * 10LL + static_cast<long long>(unit) / 2LL) / unit;
    const long long whole = tenths / 10LL;
    const long long fraction = tenths % 10LL;
    if (fraction == 0) return std::to_string(whole) + suffix;
    return std::to_string(whole) + "." + std::to_string(fraction) + suffix;
}

}  // namespace

const std::vector<int>& presets() {
    static const std::vector<int> kPresets = {
        4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576,
    };
    return kPresets;
}

int normalize(std::optional<int> value) {
    if (!value.has_value() || *value <= 0) return kDefaultTokens;
    const long long migrated =
        *value <= 100 ? static_cast<long long>(*value) * 1024LL : static_cast<long long>(*value);
    if (migrated < kMinTokens) return kMinTokens;
    if (migrated > kMaxTokens) return kMaxTokens;
    return static_cast<int>(migrated);
}

std::string compact_label(int tokens) {
    const int normalized = tokens < 0 ? 0 : tokens;
    if (normalized >= 1048576) return compact_unit_label(normalized, 1048576, "M");
    if (normalized > 1000) return compact_unit_label(normalized, 1024, "K");
    return std::to_string(normalized);
}

}  // namespace agora::core::context_budget
