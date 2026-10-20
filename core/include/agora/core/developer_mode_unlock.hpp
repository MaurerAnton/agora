#pragma once

#include <string>

namespace agora::core::developer_mode_unlock {

// Port of ui/settings/DeveloperModeUnlockPolicy.kt: the Android-style release unlock shared by
// the About screen and its unit tests. Seven taps unlock developer mode; once enabled the policy
// is a no-op.

inline constexpr int kRequiredTaps = 7;

enum class Feedback {
    None,
    RemainingTaps,
    Enabled,
    AlreadyEnabled,
};

struct Result {
    int tap_count = 0;
    int remaining_taps = 0;
    Feedback feedback = Feedback::None;
};

/**
 * advance: alreadyEnabled wins; otherwise the stored count is coerced into [0, REQUIRED_TAPS - 1]
 * before the increment, so stale or hostile values cannot overflow the counter and any count of
 * six or more unlocks on the next tap. The seventh tap resets the counter to zero.
 */
Result advance(int current_tap_count, bool already_enabled);

/** Kotlin data-class toString() of DeveloperUnlockResult. */
std::string describe(const Result& result);

}  // namespace agora::core::developer_mode_unlock
