#!/usr/bin/env python3
"""Deterministic scenario generator for util/ConchHandshake.kt.

Each scenario exercises the handshake verification with deterministic inputs.
"""
from __future__ import annotations

import json
import hashlib
import hmac
from pathlib import Path

HERE = Path(__file__).parent
OUT = HERE / "scenarios"
OUT.mkdir(exist_ok=True)

def write(name: str, scenario: dict):
    with open(OUT / (name + ".json"), "w") as f:
        json.dump(scenario, f, indent=2)
        f.write("\n")

def hmac_sha256_hex(key: str, data: str) -> str:
    return hmac.new(key.encode('utf-8'), data.encode('utf-8'), hashlib.sha256).hexdigest()

def main():
    for f in OUT.glob("*.json"):
        f.unlink()

    # Valid handshake with all required features
    api_key = "test-api-key-123"
    nonce = "nonce123"
    challenge = "challenge456"
    public_key = "dGVzdC1wdWJsaWMta2V5"  # base64url of "test-public-key"
    features = "hkdf-extract-v2,response-auth-v1,sse-sequence-v1,rejection-auth-v1"
    
    # securityPayload = "conch-handshake-v2|$public_key|$challenge|$features"
    security_payload = f"conch-handshake-v2|{public_key}|{challenge}|{features}"
    
    # signature = HMAC-SHA256(api_key, "nonce|public_key")
    payload1 = f"{nonce}|{public_key}"
    signature = hmac_sha256_hex(api_key, payload1)
    
    # features_signature = HMAC-SHA256(api_key, "nonce|securityPayload")
    payload2 = f"{nonce}|{security_payload}"
    features_signature = hmac_sha256_hex(api_key, payload2)

    # Valid handshake
    write("01-valid_full", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": features,
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

    # Invalid: empty challenge
    write("02-empty_challenge", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": features,
            "challenge": "",
            "features_signature": features_signature
        }
    })

    # Invalid: wrong challenge
    write("03-wrong_challenge", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": "different_challenge",
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": features,
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

    # Invalid: empty nonce
    write("04-empty_nonce", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": "",
            "signature": signature,
            "features": features,
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

    # Invalid: wrong signature
    write("05-wrong_signature", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": "0" * 64,
            "features": features,
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

    # Invalid: wrong features_signature
    write("06-wrong_features_signature", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": features,
            "challenge": challenge,
            "features_signature": "0" * 64
        }
    })

    # Invalid: missing required feature
    write("07-missing_feature", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": "hkdf-extract-v2,response-auth-v1",  # missing sse-sequence-v1, rejection-auth-v1
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

    # Extra feature alongside the required ones: the features string is signed over as-is, so
    # the signature must cover the extended list for the containsAll check to be reached.
    features08 = features + ",extra-feature"
    security_payload08 = f"conch-handshake-v2|{public_key}|{challenge}|{features08}"
    features_signature08 = hmac_sha256_hex(api_key, f"{nonce}|{security_payload08}")
    write("08-extra_feature", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": features08,
            "challenge": challenge,
            "features_signature": features_signature08
        }
    })

    # Invalid: signature wrong length
    write("09-signature_wrong_length", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": "abc",  # wrong length
            "features": features,
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

    # Invalid: empty features_signature
    write("10-empty_features_signature", {
        "op": "verify",
        "api_key": api_key,
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,
            "features": features,
            "challenge": challenge,
            "features_signature": ""
        }
    })

    # Invalid: wrong api key for signature
    write("11-wrong_api_key", {
        "op": "verify",
        "api_key": "different-api-key",
        "expected_challenge": challenge,
        "handshake": {
            "public_key": public_key,
            "nonce": nonce,
            "signature": signature,  # signed with original api_key
            "features": features,
            "challenge": challenge,
            "features_signature": features_signature
        }
    })

if __name__ == "__main__":
    main()