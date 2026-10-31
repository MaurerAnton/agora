# Agora public server APIs

These dependency-free receivers implement Agora's two public submission APIs:

* [`rating/`](rating/) accepts `POST /api/rating` and writes valid submissions to SQLite. It exposes no read or administration API.
* [`crash/`](crash/) accepts opt-in crash reports, applies a field allowlist, omits client IP addresses, and writes records to local JSONL storage.

Both run as one C++ binary, `agora_server` (modes `rating` and `crash`), built from `core/`;
each service directory documents its own install steps. The Python reference implementations
live in git history and their wire behaviour is pinned by `core/tests/server_submission_test.cpp`.

Dashboard code, private read APIs, administration routes, runtime databases, reports, logs, certificates, credentials, host addresses, and production virtual-host configuration are not included.
