#pragma once

#include <string>
#include <string_view>

namespace agora::core::aliases {

/**
 * CustomProviderIdentity.kt's inferModelAlias: the human-readable display fallback for a model id.
 *
 * The path tail is used (the whole id when it is all slashes), a trailing `:batch`/`:free` variant
 * suffix is dropped, and the Gemini-preview / Claude (family- or version-first) / Nova patterns
 * are tried in that order before every `-`/`_`/whitespace separated token is humanized through the
 * display-name tables (including the numeric-suffix rules, e.g. `qwen3` → `Qwen 3`).
 */
std::string infer_model_alias(std::string_view model_name);

}  // namespace agora::core::aliases