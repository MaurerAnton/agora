#!/usr/bin/env python3
"""Masks the non-deterministic trace fields, keeping each value's length in the placeholder.

Fresh traces are produced by a running JVM: ephemeral keys, random nonces and GCM ciphertexts
never repeat, so the diff can only compare the deterministic shape. The placeholder records the
*encoded length*, which keeps the sizes the Kotlin implementation draws pinned:

    generate_nonce's 12 bytes        -> "<nonce:16>"
    raw X25519 public key (32 B)     -> "<public_key_raw:43>"
    DER PKCS#8 private key (48 B)    -> "<private_key_raw:64>"
    nonce(12)+ciphertext+tag(16)     -> "<ciphertext:N>"

The C++ parity test applies the same four rules (core/tests/shell_crypto_parity_test.cpp), so a
port that draws the wrong number of bytes still fails the trace.

Usage: python3 mask_trace.py <trace-file>
"""
import re
import sys

RULES = [
    ("nonce", r"[A-Za-z0-9\-_]{10,}", "<nonce:%d>"),
    ("ciphertext", r"[A-Za-z0-9\-_]{20,}", "<ciphertext:%d>"),
    ("public_key_raw", r"[A-Za-z0-9\-_]{20,}", "<public_key_raw:%d>"),
    ("private_key_raw", r"[A-Za-z0-9+/=]{20,}", "<private_key_raw:%d>"),
]


def mask(content, key, pattern, template):
    def repl(match):
        return '"%s":"%s"' % (key, template % len(match.group(1)))

    return re.sub(r'"%s"\s*:\s*"(%s)"' % (key, pattern), repl, content)


def main(path):
    with open(path) as f:
        content = f.read()
    for key, pattern, template in RULES:
        content = mask(content, key, pattern, template)
    with open(path, "w") as f:
        f.write(content)


if __name__ == "__main__":
    main(sys.argv[1])
