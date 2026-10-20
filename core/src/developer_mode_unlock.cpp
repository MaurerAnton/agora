// Port of ui/settings/DeveloperModeUnlockPolicy.kt.
#include "agora/core/developer_mode_unlock.hpp"

namespace agora::core::developer_mode_unlock {
namespace {

constexpr int kFeedbackThreshold = 3;

const char* feedback_name(Feedback feedback) {
    switch (feedback) {
        case Feedback::None:
            return "NONE";
        case Feedback::RemainingTaps:
            return "REMAINING_TAPS";
        case Feedback::Enabled:
            return "ENABLED";
        case Feedback::AlreadyEnabled:
            return "ALREADY_ENABLED";
    }
    return "NONE";
}

}  // namespace

Result advance(int current_tap_count, bool already_enabled) {
    if (already_enabled) {
        return Result{0, 0, Feedback::AlreadyEnabled};
    }

    int clamped = current_tap_count;
    if (clamped < 0) clamped = 0;
    if (clamped > kRequiredTaps - 1) clamped = kRequiredTaps - 1;

    const int next_tap_count = clamped + 1;
    const int remaining = kRequiredTaps - next_tap_count;
    if (remaining == 0) {
        return Result{0, 0, Feedback::Enabled};
    }

    const Feedback feedback =
        remaining <= kFeedbackThreshold ? Feedback::RemainingTaps : Feedback::None;
    return Result{next_tap_count, remaining, feedback};
}

std::string describe(const Result& result) {
    std::string out = "DeveloperUnlockResult(tapCount=";
    out += std::to_string(result.tap_count);
    out += ", remainingTaps=";
    out += std::to_string(result.remaining_taps);
    out += ", feedback=";
    out += feedback_name(result.feedback);
    out += ')';
    return out;
}

}  // namespace agora::core::developer_mode_unlock
