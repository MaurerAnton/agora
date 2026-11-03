# This fork

This repository is **[MaurerAnton/agora](https://github.com/MaurerAnton/agora)**, a fork of
**[newo-ether/Agora](https://github.com/newo-ether/Agora)**. It merges upstream releases (current
upstream base: **2.2.0**, commit `da153d05`) and adds two kinds of work on top:

1. a C++ implementation of the app core, pinned to the Kotlin reference by golden traces, and
2. a small set of user-visible improvements.

This file is the single place that lists them, so fork users know exactly what differs from
upstream. Everything not listed here is expected to behave exactly like upstream.

## User-visible improvements

| Improvement | Details | Shipped in |
| --- | --- | --- |
| Provider debugging | Every provider HTTP failure names the endpoint it came from; provider settings gains a connection test. | `ci-provider-debug-2026-10-31` |
| Onboarding Skip | The Skip button stays visible on every intro page instead of waiting for the intro fade. | `ci-onboarding-skip-2026-11-01` |
| Ask-user deadline | Keystrokes restart the ask-user deadline instead of losing the prompt; drafted input survives, and a retry is held in the queue instead of being dropped. | `ci-issue116-2026-11-02` |
| Script detection | Assistant output is checked against every disallowed script, not only Han. | `ci-issue116-2026-11-02` |
| Tail-only regeneration | Regenerating a response rewrites only the text tail in-run and keeps every side effect. | `ci-issue116-2026-11-02` |
| WorkManager 2.12.0 | Fixes the `SystemForegroundService` crash on current Android versions. | release builds |
| Tool-score parsing | Tool scores are parsed without `float` `from_chars`, avoiding locale-dependent behavior. | release builds |
| C++ core | See the next section. | `ci-cpp-port-2026-11-03` |

## C++ core (`core/`)

The fork carries a second, independent implementation of the app core in C++:

* The golden-stand tooling (trace dumps, scenario generators) exists twice: the Kotlin
  implementation produces reference traces, the C++ implementation must reproduce them
  **byte-for-byte**. `ctest` runs that comparison for every ported module.
* An upstream merge is followed by re-freezing only the traces whose semantics changed, then
  porting until `ctest` is green again. That is how the fork keeps the C++ side honest instead of
  drifting from the Kotlin reference.
* The C++ library has no external dependencies and builds warning-free under `-Wall -Wextra`.

## Staying informed

* Releases are pre-releases named `APK: …` and ship both the dev and the F-Droid APK, built from
  the exact green CI run.
* CI runs on every push: unit tests, the C++ core build and `ctest`, then the APKs.
* Known CI flake: the `Check Play flavor behavior` step has a 5-minute budget and occasionally
  exceeds it on slow GitHub runners. A re-run of the failed job is the fix; it is unrelated to
  code under test.

## Sync policy

* **Upstream first.** When upstream changes behavior, the Kotlin code is taken as-is; the C++ port
  follows, and frozen traces are regenerated only after that.
* Fork-only improvements are kept in separate commits with their own tests and are listed above.
* Fork-only code stays out of upstream packages where possible: the C++ core lives entirely under
  `core/`, and user-visible changes are scoped commits rather than broad rewrites.
