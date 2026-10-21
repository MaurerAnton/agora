#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agora::core::settings_reset {

// Port of data/PortableSettingsReset.kt: the portable allowlist a settings reset clears.
//
// The list is generated from the Kotlin policy (see the stand's gen_reset_keys.py), so the C++
// port and the Kotlin source cannot drift apart. The names are the DataStore preference names.

/** Every preference the reset removes, in the Kotlin source order. */
const std::vector<std::string>& removed_preference_names();

/** The preference the reset rewrites to an empty container instead of removing. */
inline constexpr std::string_view kModelProviderNamesKey = "model_provider_names_json";
inline constexpr std::string_view kEmptyJsonObject = "{}";

/** The storage the reset writes through (the DataStore preferences contract). */
class PreferenceStore {
public:
    virtual ~PreferenceStore() = default;

    /** Removes one preference by name. */
    virtual void remove(std::string_view key) = 0;

    /** Writes one preference by name. */
    virtual void set_raw(std::string_view key, std::string_view value) = 0;
};

/**
 * clearPortableSettings: removes the portable allowlist and rewrites the provider-name map.
 * Secrets, device-local state, sandbox and lifecycle metadata stay untouched.
 */
void clear_portable_settings(PreferenceStore& store);

}  // namespace agora::core::settings_reset
