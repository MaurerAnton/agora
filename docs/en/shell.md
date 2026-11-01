# Remote Shell

Open **Settings → Shell Devices** to configure Conch or SSH devices used by agentic tools. Give each device a unique name and select Save to create it. Names ignore surrounding whitespace and case; `Local Sandbox` is reserved for the on-device sandbox. Duplicate names cannot be saved. Leaving a new editor without saving creates no device.

## Conch

Configure the server URL, optional API key, and command-confirmation policy. With an API key, Agora fetches the server's signed public key, derives an ephemeral shared key, and sends authenticated AES-GCM requests. With a blank key, the application payload is plain JSON and transport confidentiality depends on HTTPS.

Foreground commands are created as durable server jobs and waited on for a bounded period. If the wait expires, the command is not killed or replayed: Agora returns a `job_id` that can be inspected, waited on, stopped, and acknowledged later. Conch tools also support remote read, write, edit, glob, grep, and image viewing.

## SSH

Configure the host, port, and username, then choose password authentication or select **Private Key** and **Import Private Key** to import a file no larger than 256 KiB. For an encrypted key, enter its passphrase. Use **Verify & Pin Host Key** to review the server fingerprint, then select **Save** to store the device and credentials. Once pinned, later connections must match that host key. Credentials are stored encrypted on the device. Structural Settings exports exclude passwords, private keys, and key passphrases; native `.agora` backups include them only with the explicit include-secrets option. Direct SSH transport is separate from Agora's shared HTTP proxy.

## Confirmation

Commands can require confirmation or be always allowed per configured server. Treat both shell types as high-trust capabilities: the remote account's permissions define what the model can change.

See [Agentic Tools](tools.md), [Network Proxy](proxy.md), and [Privacy & Security](privacy.md).
