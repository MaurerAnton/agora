# database-compatibility golden stand

Parity pin for `data/local/DatabaseCompatibility.kt` — the fail-closed inspection that runs before
Room is allowed to open the durable database. Missing and supported databases may open; future
versions and unreadable files stay closed until the user explicitly clears them. The source
compiles **verbatim from the app tree** (zero imports); the canonical trace is frozen under
`golden/` and the shipped port must reproduce every trace byte for byte
(`core/tests/database_compatibility_parity_test.cpp`).

## What the port is

`agora::core::database_compatibility` (`core/src/database_compatibility.cpp`):

| Kotlin | C++ under test |
| --- | --- |
| `DatabaseCompatibility` sealed variants | `database_compatibility::Classification` + `Kind` |
| `classifyDatabaseCompatibility(exists, current, read)` | `database_compatibility::classify` |
| `DatabaseCompatibility.canOpen` | `database_compatibility::can_open` |
| data-class / data-object `toString()` | `database_compatibility::describe` |

The Kotlin reader is a `() -> Int` that may throw; the port models the same boundary with an
optional stored version plus an optional exception description produced by the platform.

## What is pinned

* **missing database** — returns `Missing` even when the stored-version reader would throw; the
  short-circuit runs before the `try`.
* **supported** — `storedVersion <= currentVersion`, including equal versions, zero, negative and
  `Int.MIN_VALUE` stored values.
* **future version** — only strictly newer stored versions (`5→6`, `5→99`, `-1→0`,
  `Int.MIN_VALUE→Int.MAX_VALUE`) and the carried supported version.
* **unreadable** — reader exceptions become `Unreadable` with the exception description;
  `IllegalStateException` (with message, without message, empty message), `NullPointerException`,
  `RuntimeException`, `IOException` and `IllegalArgumentException` are all pinned.
* **canOpen** — true exactly for `Missing` and `Supported`, false for `FutureVersion` and
  `Unreadable`.

## Documented boundaries

* The Kotlin catch clause is `Exception`, not `Throwable`: `Error`s propagate and never become
  `Unreadable`. The scenarios only throw `Exception` subclasses, matching every real I/O failure
  path.
* Exception `toString()` formatting is JVM-supplied (`class: message`, or the bare class name when
  the message is null); the port stores the description as an opaque platform string and the
  scenario driver builds it the same way Kotlin does. Custom exceptions overriding `toString()`
  are out of scope.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (23 cases); rerun
`./run_golden.sh --freeze` afterwards.
