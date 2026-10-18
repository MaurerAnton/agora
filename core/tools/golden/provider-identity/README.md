# provider-identity golden stand

Parity pin for the native port of the custom provider identity policy
(`CustomProviderIdentity.kt`). Every scenario under `scenarios/` runs through the
**real Kotlin sources compiled from the app tree** and the canonical trace is
frozen under `golden/`; the C++ port (`agora::core::provider_support`,
test `core/tests/provider_names_parity_test.cpp`) must reproduce every trace
byte for byte.

The identity policy covers:
- `legacyId(name)`: derives a stable UUID from a provider name
- `isStableId(value)`: checks if a string matches the stable ID pattern
- `replaceStableIdsForDisplay(text, replacement)`: replaces stable IDs in text
- `normalize(providers, occupiedIds, newId)`: normalizes a list of provider
  configurations, assigning stable IDs and generating migrations
- `generateUniqueId(provider, usedIds, idGenerator)`: generates a unique
  stable ID for a provider
- `remapProviderReference(ref, migrations)`: rewrites provider references
  in model IDs
- `canonicalCustomModelId(modelId, providers)`: resolves a model ID to its
  canonical form using provider identity

The reserved provider names (10 built-ins) are matched case-insensitively
after trimming. `hasConflict` returns true for disallowed names or duplicates
(case-insensitive, trimmed), except the `currentName` entry which is exempt
via exact case-sensitive match. `sanitize` keeps the first occurrence of each
name and quarantines the rest with their original untrimmed strings.

All code compiles verbatim from the app tree; the driver calls the real
functions directly — no transcription.