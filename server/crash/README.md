# Agora crash-report receiver

This dependency-free Python service receives only the anonymous, explicitly submitted crash report used by Agora:

```text
POST /crash
```

The Android client stores one pending report locally after an uncaught exception. On the next launch it asks the user whether to send it; there is no automatic upload.

The public Nginx route proxies to loopback port 8092. The receiver accepts at most 64 KiB, rate-limits accepted submissions, applies the source-defined field allowlist, omits client IP addresses from storage, appends sanitized JSON records to `/var/lib/agora-crash/crashes.jsonl`, and rotates at approximately 50 MiB. The current allowlist stores stack trace, app/version data, coarse Android/device model data, and timestamps; it does not store conversation content, credentials, or Android device identifiers.

## Installation

Review the service account in `agora-crash.service` for the target host, then install the exact checked-in files:

```sh
sudo install -d -o newoether -g newoether -m 0750 /var/lib/agora-crash
sudo install -d -o root -g root -m 0755 /opt/agora-crash
sudo install -o root -g root -m 0755 agora-crash.py /opt/agora-crash/
sudo install -o root -g root -m 0644 agora-crash.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now agora-crash
```

Copy the location from `nginx-crash.location` into the public TLS virtual host, validate the Nginx configuration, then reload Nginx. Runtime reports, logs, host identities, certificates, and credentials must not be committed.
