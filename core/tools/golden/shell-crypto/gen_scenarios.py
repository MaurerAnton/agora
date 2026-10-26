#!/usr/bin/env python3
"""Deterministic scenario generator for util/ShellCrypto.kt.

Each scenario exercises one function with deterministic inputs and freezes the output trace.
Scenarios cover: key generation, encoding/decoding, ECDH+HKDF, AES-GCM encrypt/decrypt,
HMAC signing, SHA-256, response auth, nonce generation.
"""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = Path(__file__).parent
OUT = HERE / "scenarios"

# Deterministic seeds for reproducible randomness
# These are generated using cryptography library with deterministic seeds
PRIVATE_KEY_PKCS8 = "MC4CAQAwBQYDK2VuBCIEIAAHDhUcIyoxOD9GTVRbYmlwd36FjJOaoaivtr3Ey9LZ"
PUBLIC_KEY_RAW = "F32PIx9sq4nqA_kNLy1T3ZuhPHiUfV2FuD1JRRTgtxQ"

# AES key for encrypt/decrypt tests
AES_KEY_32 = base64.urlsafe_b64encode(bytes([i % 256 for i in range(32)])).decode('ascii').rstrip('=')
AES_KEY_32_ALT = base64.urlsafe_b64encode(bytes([(i + 1) % 256 for i in range(32)])).decode('ascii').rstrip('=')

# Session key for response auth
SESSION_KEY_32 = base64.urlsafe_b64encode(bytes([i % 256 for i in range(32)])).decode('ascii').rstrip('=')


def write(name: str, scenario: dict):
    with open(OUT / (name + ".json"), "w") as f:
        json.dump(scenario, f, indent=2)
        f.write("\n")


def main():
    OUT.mkdir(exist_ok=True)
    for f in OUT.glob("*.json"):
        f.unlink()

    # ===== Key Generation =====
    write("01-generate_ephemeral_keypair", {
        "op": "generate_ephemeral_keypair",
    })

    # ===== Public Key Encoding/Decoding =====
    write("02-encode_public_key", {
        "op": "encode_public_key",
        "public_key_raw": PUBLIC_KEY_RAW,
    })

    write("03-decode_public_key_valid", {
        "op": "decode_public_key",
        "base64url_key": PUBLIC_KEY_RAW,
    })

    write("04-decode_public_key_invalid_length", {
        "op": "decode_public_key",
        "base64url_key": base64.urlsafe_b64encode(b"x" * 16).decode('ascii').rstrip('='),
    })

    # ===== ECDH + HKDF =====
    write("05-derive_aes_key", {
        "op": "derive_aes_key",
        "private_key_pkcs8": PRIVATE_KEY_PKCS8,
        "public_key_raw": PUBLIC_KEY_RAW,
    })

    # ===== AES-256-GCM Encrypt/Decrypt =====
    write("06-encrypt_decrypt_roundtrip", {
        "op": "encrypt_decrypt",
        "key": AES_KEY_32,
        "plaintext": "Hello, Conch!",
    })

    write("07-encrypt_empty_plaintext", {
        "op": "encrypt_decrypt",
        "key": AES_KEY_32,
        "plaintext": "",
    })

    write("08-encrypt_large_plaintext", {
        "op": "encrypt_decrypt",
        "key": AES_KEY_32,
        "plaintext": "x" * 10000,
    })

    write("09-decrypt_ciphertext_too_short", {
        "op": "decrypt",
        "key": AES_KEY_32,
        "ciphertext": base64.urlsafe_b64encode(b"short").decode('ascii').rstrip('='),
    })

    write("10-decrypt_wrong_key", {
        "op": "decrypt",
        "key": AES_KEY_32_ALT,
        "ciphertext": "PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER",
        "plaintext": "test",
    })

    # ===== HMAC-SHA256 Sign =====
    write("11-sign", {
        "op": "sign",
        "api_key": "test-api-key-123",
        "timestamp": 1234567890,
        "method": "POST",
        "path": "/execute",
        "body_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "nonce": "nonce123",
        "client_pub_key": "client-pub-key",
    })

    # ===== SHA-256 =====
    write("12-sha256_hex", {
        "op": "sha256_hex",
        "data": "hello world",
    })

    # ===== Response Auth =====
    write("13-response_signature", {
        "op": "response_signature",
        "session_key": SESSION_KEY_32,
        "status": 200,
        "body": '{"result": "ok"}',
    })

    write("14-verify_response_signature_valid", {
        "op": "verify_response_signature",
        "session_key": SESSION_KEY_32,
        "status": 200,
        "body": '{"result": "ok"}',
        "signature": "PLACEHOLDER_WILL_BE_FILLED_BY_DRIVER",
    })

    write("15-verify_response_signature_invalid", {
        "op": "verify_response_signature",
        "session_key": SESSION_KEY_32,
        "status": 200,
        "body": '{"result": "ok"}',
        "signature": "0" * 64,
    })

    # ===== Nonce Generation =====
    write("16-generate_nonce", {
        "op": "generate_nonce",
    })

    # ===== Base64 URL-safe encode/decode =====
    write("17-base64_url_encode", {
        "op": "base64_url_encode",
        "input": "hello world",
    })

    write("18-base64_url_decode", {
        "op": "base64_url_decode",
        "input": "aGVsbG8gd29ybGQ",
    })

    write("19-base64_url_decode_with_padding", {
        "op": "base64_url_decode",
        "input": "aGVsbG8gd29ybGQ=",
    })

    write("20-base64_url_encode_url_safe_chars", {
        "op": "base64_url_encode",
        "input": "\xfb\xef\xbe\xde",
    })

    # ===== Failure-mode pins =====
    # A real ciphertext of "test" under AES_KEY_32 so the decryption runs with the WRONG key
    # and the GCM auth tag check fails (scenario 10 only exercises the placeholder branch).
    key_bytes = bytes([i % 256 for i in range(32)])
    nonce = bytes([0x11] * 12)
    combined = nonce + AESGCM(key_bytes).encrypt(nonce, b"test", None)
    write("21-decrypt_auth_failure", {
        "op": "decrypt",
        "key": AES_KEY_32_ALT,
        "ciphertext": base64.urlsafe_b64encode(combined).decode('ascii').rstrip('='),
    })

    # The same ciphertext under the RIGHT key: pins AES-256-GCM against an implementation that
    # is neither the JVM nor C++ (Python's cryptography), so a self-consistent but wrong port of
    # the cipher cannot pass the trace.
    write("27-decrypt_cross_implementation", {
        "op": "decrypt",
        "key": AES_KEY_32,
        "ciphertext": base64.urlsafe_b64encode(combined).decode('ascii').rstrip('='),
    })

    # Passes the nonce+1 pre-check (15 bytes) but the body is shorter than the 16-byte GCM tag.
    write("22-decrypt_body_shorter_than_tag", {
        "op": "decrypt",
        "key": AES_KEY_32,
        "ciphertext": base64.urlsafe_b64encode(b"\x00" * 12 + b"abc").decode('ascii').rstrip('='),
    })

    # The two edges of the tag-length boundary: body of exactly 15 (last short-buffer byte)
    # and exactly 16 (first byte the tag check consumes).
    write("25-decrypt_body_15_bytes", {
        "op": "decrypt",
        "key": AES_KEY_32,
        "ciphertext": base64.urlsafe_b64encode(b"\x00" * 12 + b"b" * 15).decode('ascii').rstrip('='),
    })
    write("26-decrypt_body_16_bytes_bad_tag", {
        "op": "decrypt",
        "key": AES_KEY_32,
        "ciphertext": base64.urlsafe_b64encode(b"\x00" * 12 + b"b" * 16).decode('ascii').rstrip('='),
    })

    # base64url decode failure: java.util.Base64 (vendored by platform/Base64.java) rejects the
    # first character outside the alphabet.
    write("23-decrypt_invalid_base64", {
        "op": "decrypt",
        "key": AES_KEY_32,
        "ciphertext": "!!!",
    })

    write("24-decode_public_key_invalid_base64", {
        "op": "decode_public_key",
        "base64url_key": "!!",
    })


if __name__ == "__main__":
    OUT.mkdir(exist_ok=True)
    for f in OUT.glob("*.json"):
        f.unlink()
    main()
