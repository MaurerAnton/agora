#!/usr/bin/env python3
"""Writes the deterministic Conch network / bounded-wire scenarios."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCENARIOS = HERE / "scenarios"

CONTROL_LIMIT = 64 << 10
RESPONSE_LIMIT = 40 << 20


def write(name: str, value: dict) -> None:
    path = SCENARIOS / f"{name}.json"
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def response(code: int, body: str, headers=None, declared_size=None) -> dict:
    value = {"code": code, "body": body, "headers": headers or []}
    if declared_size is not None:
        value["declaredSize"] = declared_size
    return value


def main() -> None:
    SCENARIOS.mkdir(exist_ok=True)
    for old in SCENARIOS.glob("*.json"):
        old.unlink()

    write("01-get-success", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key?challenge=abc",
        "headers": [["X-Test", "1"]],
        "response": response(200, "hello", [["X-Conch-Response-Signature", "sig-1"]]),
    })
    write("02-get-duplicate-header", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key?challenge=abc",
        "headers": [["X-Test", "1"], ["x-test", "2"]],
        "response": response(200, "hello", [["X-Conch-Response-Signature", "sig-1"]]),
    })
    write("03-get-error-body-over-control", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key",
        "headers": [],
        "response": response(500, "", declared_size=CONTROL_LIMIT + 1),
    })
    write("04-get-error-body-at-control", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key",
        "headers": [],
        "response": response(500, "", declared_size=CONTROL_LIMIT),
    })
    write("05-get-success-control-plus-one", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key",
        "headers": [],
        "response": response(200, "", declared_size=CONTROL_LIMIT + 1),
    })
    write("06-get-success-over-response", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key",
        "headers": [],
        "response": response(200, "", declared_size=RESPONSE_LIMIT + 1),
    })
    write("07-post-default-timeout", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{\"a\":1}",
        "headers": [["Content-Type", "text/plain"]],
        "response": response(200, "ok", [["X-Conch-Response-Signature", "sig-2"]]),
    })
    write("08-post-explicit-timeout", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{\"a\":1}",
        "headers": [],
        "timeoutMillis": 1234,
        "response": response(200, "ok"),
    })
    write("09-post-zero-timeout", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "timeoutMillis": 0,
        "response": response(200, "ok"),
    })
    write("10-post-negative-timeout", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "timeoutMillis": -1,
        "response": response(200, "ok"),
    })
    write("11-post-signature-lowercase", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "response": response(200, "ok", [["x-conch-response-signature", "sig-lower"]]),
    })
    write("12-post-duplicate-signature", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "response": response(200, "ok", [
            ["X-Conch-Response-Signature", "first"],
            ["x-conch-response-signature", "second"],
        ]),
    })
    write("13-get-empty-signature", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key",
        "headers": [],
        "response": response(200, "hello"),
    })
    write("14-post-utf8-body", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{\"text\":\"héllo 😀\"}",
        "headers": [["X-Encryption", "v2"]],
        "response": response(200, "ok"),
    })
    write("15-post-error-at-response-limit", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "response": response(500, "", declared_size=RESPONSE_LIMIT),
    })
    write("16-get-utf8-response", {
        "op": "getTextResponse",
        "url": "https://conch.test/public-key",
        "headers": [],
        "response": response(200, "héllo 😀", [["X-Conch-Response-Signature", "sig-utf8"]]),
    })

    write("17-read-text-small", {
        "op": "readBoundedWireText", "limit": 8, "body": "héllo",
    })
    write("18-read-text-at-limit", {
        "op": "readBoundedWireText", "limit": 6, "body": "héllo",
    })
    write("19-read-text-over-limit", {
        "op": "readBoundedWireText", "limit": 5, "body": "héllo",
    })
    write("20-read-text-zero-limit", {
        "op": "readBoundedWireText", "limit": 0, "body": "abc",
    })
    write("21-read-text-declared-over", {
        "op": "readBoundedWireText", "limit": 10, "body": "abc", "declaredSize": 11,
    })
    write("22-read-line-crlf", {
        "op": "readBoundedWireLine", "limit": 10, "body": "abc\r\nrest",
    })
    write("23-read-line-lf", {
        "op": "readBoundedWireLine", "limit": 10, "body": "abc\nrest",
    })
    write("24-read-line-too-long", {
        "op": "readBoundedWireLine", "limit": 3, "body": "abcdef",
    })
    write("25-read-line-incomplete", {
        "op": "readBoundedWireLine", "limit": 10, "body": "abc",
    })
    write("26-read-line-empty", {
        "op": "readBoundedWireLine", "limit": 10, "body": "",
    })
    write("27-read-line-zero-limit", {
        "op": "readBoundedWireLine", "limit": 0, "body": "abc",
    })
    write("28-post-success-control-plus-one", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "response": response(200, "", declared_size=CONTROL_LIMIT + 1),
    })
    write("29-post-success-over-response", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "response": response(200, "", declared_size=RESPONSE_LIMIT + 1),
    })
    write("30-post-success-at-response", {
        "op": "postTextResponse",
        "url": "https://conch.test/execute",
        "body": "{}",
        "headers": [],
        "response": response(200, "", declared_size=RESPONSE_LIMIT),
    })


if __name__ == "__main__":
    main()
