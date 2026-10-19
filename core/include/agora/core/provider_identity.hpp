#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::provider_identity {

/**
 * Port of data/CustomProviderIdentity.kt and the parts of data/CustomProviderNamePolicy.kt it
 * depends on.
 *
 * The policy exists because a custom provider's display name is mutable but its id owns model
 * identity. Names are still name-keyed for backward compatibility, so a custom name may not shadow
 * a built-in provider; and a persisted id, once minted, never changes on rename.
 */

/** data/CustomProviderConfig.kt, restricted to the fields identity cares about. */
struct Provider {
    std::string name;
    std::string id;
    /** A LinkedHashSet in Kotlin: order is observable through the emitted migrations. */
    std::vector<std::string> legacy_names;
};

struct Migration {
    std::string legacy_reference;
    std::string provider_id;
};

struct Normalization {
    std::vector<Provider> providers;
    std::vector<Migration> migrations;
};

struct Sanitization {
    std::vector<Provider> accepted;
    std::vector<Provider> rejected;
};

/**
 * An ordered string map. Kotlin iterates LinkedHashMap entry order, which is observable: when two
 * legacy prefixes tie on length, the first one in the map wins. std::map would silently sort them.
 */
using ReferenceMap = std::vector<std::pair<std::string, std::string>>;

/** The app's injected id generator. It receives the provider being normalised. */
using IdGenerator = std::function<std::string(const Provider&)>;

/**
 * The retry path's entropy. generateUniqueId() calls the policy's own newId() for attempts after
 * the first, so a faithful port must call a second source too. It is a parameter rather than a
 * hidden call so the core never reads entropy a test cannot control.
 */
using RandomIdSource = std::function<std::string()>;

/** `custom-provider-` + 8-4-4-4-12 hex, version 1..5, variant 89ab, compared case-insensitively. */
bool is_stable_id(std::string_view value);

/** UUID v3 (MD5 name-based) over "agora/custom-provider/legacy/" + trim(name).lowercase(). */
std::string legacy_id(std::string_view name);

/** Rewrites every stable id found inside arbitrary text, leaving near misses untouched. */
std::string replace_stable_ids_for_display(
    std::string_view text, const std::function<std::string(std::string_view)>& replacement);

/** `id` when it is a stable id, otherwise the name: CustomProviderConfig.providerId. */
std::string provider_id(const Provider& provider);

/** Non-empty after trimming and not one of the ten built-in provider names. */
bool is_allowed_name(std::string_view name);

/**
 * A duplicate of an already accepted name, case-insensitively after trimming. `current_name` marks
 * the entry being edited and is exempt by exact, case-sensitive match.
 */
bool has_conflict(std::string_view name, const std::vector<std::string>& existing_names,
                  std::optional<std::string> current_name = std::nullopt);

/** Drops blank and reserved names and keeps the first of each duplicate; rejected entries keep
 * their original, untrimmed names so name-keyed settings are never rewritten. */
Sanitization sanitize(const std::vector<Provider>& configs);

/**
 * Assigns every accepted provider a stable id, keeping an existing one when it is valid and not
 * already taken, and reports the legacy references each rename orphaned.
 */
Normalization normalize(const std::vector<Provider>& raw_providers,
                        const std::vector<std::string>& occupied_ids,
                        const IdGenerator& id_generator, const RandomIdSource& random_id);

/**
 * Rewrites a reference onto its longest migrated prefix. The prefix must be followed by ':' so a
 * shared prefix cannot swallow a longer provider id.
 */
std::string remap_provider_reference(std::string_view reference, const ReferenceMap& migrations);

/**
 * Two passes: untouched aliases first, then migrated ones via putIfAbsent, so an alias that already
 * exists under the new reference is not overwritten.
 */
ReferenceMap remap_model_preference_keys(const ReferenceMap& aliases,
                                         const ReferenceMap& migrations);

/** Resolves a model id onto the owning custom provider's current id. */
std::string canonical_custom_model_id(std::string_view model_id,
                                      const std::vector<Provider>& custom_providers);

}  // namespace agora::core::provider_identity
