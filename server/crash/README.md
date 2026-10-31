# Agora crash-report receiver

This dependency-free C++ service receives only the anonymous, explicitly submitted crash report used by Agora:

```text
POST /crash
```

The Android client stores one pending report locally after an uncaught exception. On the next launch it asks the user whether to send it; there is no automatic upload.

The public Nginx route proxies to loopback port 8092. The receiver accepts at most 64 KiB, rate-limits accepted submissions, applies the source-defined field allowlist, omits client IP addresses from storage, appends sanitized JSON records to `/var/lib/agora-crash/crashes.jsonl`, and rotates at approximately 50 MiB. The current allowlist stores stack trace, runtime package name, app/version data, coarse Android/device model data, and timestamps; it does not store conversation content, credentials, or Android device identifiers.

## Installation

Review the service account in `agora-crash.service` for the target host, then install the exact checked-in files:

```sh
# Build the receiver once (needs cmake, a C++ compiler, libsqlite3-dev, libcurl4-openssl-dev);
# the same binary serves both services, each install keeps a copy next to its unit.
cmake -S core -B core-build && cmake --build core-build --target agora_server

sudo install -d -o newoether -g newoether -m 0750 /var/lib/agora-crash
sudo install -d -o root -g root -m 0755 /opt/agora-crash
sudo install -o root -g root -m 0755 core-build/agora_server /opt/agora-crash/
sudo install -o root -g root -m 0644 server/crash/agora-crash.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now agora-crash
```

Copy the location from `nginx-crash.location` into the public TLS virtual host (or run
`agora_server patch-nginx /etc/nginx/sites-available/<site>`, which inserts the same block
idempotently), validate the Nginx configuration, then reload Nginx. Runtime reports, logs, host
identities, certificates, and credentials must not be committed.

## Optional submission messages

Set `AGORA_SUBMISSION_MESSAGES` in the service environment to an administrator-managed,
UTF-8 JSON file outside the checkout. Leave it unset to retain the ordinary success response.

The file maps exact runtime package names to a message object, or `null` to omit a message:

```json
{
  "org.example.app": {
    "id": "feedback-2026-01",
    "title": "Thank You",
    "body": "Your submission has been received.",
    "buttonText": "OK"
  }
}
```

The file is read for each accepted submission, so replacing it atomically updates messages
without restarting the receiver. Missing, malformed, unreadable, or oversized configurations
produce no message and never reject an otherwise successful submission. The file is capped at
1 MiB; id/title/body/buttonText limits are 128/200/8000/80 characters. The first three fields
are required nonblank strings; buttonText is optional. Unknown fields are not returned.

The response's optional `message` object is plain text. Clients display a dismissible dialog
after success and remember displayed IDs locally across rating and crash submissions. Use a
new ID for a new message. No message causes the existing success presentation.
Package metadata is client-supplied routing information, not an authentication mechanism.
