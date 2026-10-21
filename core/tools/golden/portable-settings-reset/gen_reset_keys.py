#!/usr/bin/env python3
"""Regenerates the C++ reset key list from data/PortableSettingsReset.kt.

The Kotlin policy removes the portable keys by their schema constant; the port removes them by
their preference name. This script maps one onto the other (through the generated
SchemaKeys.kt) so the C++ list can never drift from the Kotlin source.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
RESET = os.path.join(ROOT, "app", "src", "main", "java", "com", "newoether", "agora", "data",
                     "PortableSettingsReset.kt")
SCHEMA_KEYS = os.path.join(HERE, "SchemaKeys.kt")
OUT = os.path.join(ROOT, "core", "include", "agora", "core", "portable_settings_reset.hpp")

REMOVE = re.compile(r'prefs\.remove\(([A-Z0-9_]+)\)')


def main() -> int:
    reset_source = open(RESET, encoding="utf-8").read()
    schema_source = open(SCHEMA_KEYS, encoding="utf-8").read()

    names = {constant: literal for constant, literal in re.findall(
        r'com\.newoether\.agora\.data\.([A-Z0-9_]+)\.name,\s*//\s*"([a-z0-9_]+)"',
        schema_source)}
    if not names:
        print("error: no schema keys found in %s" % SCHEMA_KEYS, file=sys.stderr)
        return 2

    removed = REMOVE.findall(reset_source)
    if not removed:
        print("error: no removals found in %s" % RESET, file=sys.stderr)
        return 2

    literals = []
    for constant in removed:
        if constant not in names:
            print("error: %s is not a schema key" % constant, file=sys.stderr)
            return 2
        literal = names[constant]
        if literal in literals:
            print("error: %s is removed twice" % constant, file=sys.stderr)
            return 2
        literals.append(literal)

    header = '''#pragma once

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
'''
    with open(OUT, "w", encoding="utf-8") as handle:
        handle.write(header)

    cpp = os.path.join(ROOT, "core", "src", "portable_settings_reset.cpp")
    body = '''// Port of data/PortableSettingsReset.kt.
//
// The list of removed preferences is generated from the Kotlin policy (the stand's
// gen_reset_keys.py writes it into the header), so the two cannot drift apart.
#include "agora/core/portable_settings_reset.hpp"

namespace agora::core::settings_reset {

namespace {

// GENERATED from data/PortableSettingsReset.kt -- do not edit.
const std::vector<std::string>& removed_names() {
    static const std::vector<std::string> names = {
'''
    for literal in literals:
        body += '        "%s",\n' % literal
    body += '''    };
    return names;
}

}  // namespace

const std::vector<std::string>& removed_preference_names() { return removed_names(); }

void clear_portable_settings(PreferenceStore& store) {
    for (const std::string& name : removed_names()) {
        if (name == kModelProviderNamesKey) continue;  // rewritten below, not removed
        store.remove(name);
    }
    store.set_raw(kModelProviderNamesKey, kEmptyJsonObject);
}

}  // namespace agora::core::settings_reset
'''
    with open(cpp, "w", encoding="utf-8") as handle:
        handle.write(body)
    print("Wrote %d removed keys to %s and %s" % (len(literals), OUT, cpp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
