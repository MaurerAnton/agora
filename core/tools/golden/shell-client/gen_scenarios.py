#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the shell-client stand."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")
scenarios = []

SERVER = "http://conch.local:8080"
KEY = "secret-api-key"
FIXED_PUB = "ChlxST1g8D4sd0DzZmQdk76qjaeoeyJxGaY7a-g1W3k"


def add(name, scenario):
    scenarios.append((name, scenario))


def body(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def fetch(name, **kwargs):
    scenario = {"op": "fetch_public_key", "server_url": SERVER, "api_key": KEY}
    scenario.update(kwargs)
    add(name, scenario)


def prepare(name, **kwargs):
    scenario = {
        "op": "prepare_request", "server_url": SERVER, "api_key": KEY,
        "command": "ls -la", "timeout_ms": 5000, "workdir": "",
    }
    scenario.update(kwargs)
    add(name, scenario)


def decrypt(name, **kwargs):
    scenario = {
        "op": "decrypt_sse", "server_url": SERVER, "api_key": KEY,
        "command": "ls", "timeout_ms": 5000,
        "run_fetch_first": False, "prepare_first": False,
        "seal_for_session": False, "data": "x",
    }
    scenario.update(kwargs)
    add(name, scenario)


def file_op(name, op, **kwargs):
    scenario = {"op": op, "server_url": SERVER, "api_key": ""}
    scenario.update(kwargs)
    add(name, scenario)


def job(name, op, **kwargs):
    scenario = {"op": op, "server_url": SERVER, "api_key": ""}
    scenario.update(kwargs)
    add(name, scenario)


# ---- fetch_public_key ----------------------------------------------------------------------------
fetch("fetch-success")
fetch("fetch-blank-key", api_key="")
fetch("fetch-wrong-sign-key", handshake_sign_with="other-key")
fetch("fetch-missing-features",
      handshake_features="hkdf-extract-v2,response-auth-v1")
fetch("fetch-challenge-mismatch", handshake_challenge="different")
fetch("fetch-http-failure", handshake_http_failure=True,
      handshake_failure_code=503, handshake_failure_body="maintenance")
fetch("fetch-http-empty", handshake_http_failure=True,
      handshake_failure_code=500, handshake_failure_body="")
fetch("fetch-http-long-body", handshake_http_failure=True,
      handshake_failure_code=502, handshake_failure_body="x" * 300)
fetch("fetch-unknown-host", transport_failure="unknown_host",
      transport_failure_detail="conch.local")
fetch("fetch-connect", transport_failure="connect",
      transport_failure_detail="connection refused")
fetch("fetch-socket-timeout", transport_failure="socket_timeout",
      transport_failure_detail="read timed out")
fetch("fetch-interrupted-io", transport_failure="interrupted_io",
      transport_failure_detail="interrupted")
fetch("fetch-ssl", transport_failure="ssl", transport_failure_detail="bad cert")
fetch("fetch-plain-exception", transport_failure="plain",
      transport_failure_detail="boom")
fetch("fetch-cached-invalid", cached_public_key="garbage-not-a-key")
fetch("fetch-decode-key-failure", handshake_public_key="AAAA")

# ---- prepare_request -----------------------------------------------------------------------------
prepare("prepare-plain", api_key="")
prepare("prepare-plain-workdir", api_key="", workdir="/home/user")
prepare("prepare-encrypted", run_fetch_first=True)
prepare("prepare-no-fetch")
prepare("prepare-plain-command-unicode", api_key="", command="echo héllo 世界")

# ---- decrypt_sse ---------------------------------------------------------------------------------
decrypt("decrypt-no-session")
decrypt("decrypt-roundtrip", run_fetch_first=True, prepare_first=True,
        seal_for_session=True, plaintext="hello sse")
decrypt("decrypt-raw-short", run_fetch_first=True, prepare_first=True,
        data="AAAA")

# ---- file_read -----------------------------------------------------------------------------------
file_op("read-plain-basic", "file_read", path="/tmp/a.txt", offset=0, limit=0,
        response={"code": 200, "body": body(
            {"content": "hello", "size": 5, "lines": 1, "totalLines": 1})})
file_op("read-offset-limit", "file_read", path="/tmp/a.txt", offset=6, limit=5,
        response={"code": 200, "body": body({"content": "World", "size": 11})})
file_op("read-limit-zero", "file_read", path="/a", offset=0, limit=0,
        response={"code": 200, "body": body({"content": "abc", "size": 3})})
file_op("read-limit-over-max", "file_read", path="/a", offset=0, limit=2_000_000,
        response={"code": 200, "body": body({"content": "abc", "size": 3})})
file_op("read-offset-negative", "file_read", path="/a", offset=-3, limit=0,
        response={"code": 200, "body": body({"content": "abc", "size": 3})})
file_op("read-offset-past-end", "file_read", path="/a", offset=10, limit=0,
        response={"code": 200, "body": body({"size": 5})})
file_op("read-error-field", "file_read", path="/missing", offset=0, limit=0,
        response={"code": 200, "body": body(
            {"content": "", "error": "File not found"})})
file_op("read-error-null", "file_read", path="/a", offset=0, limit=0,
        response={"code": 200, "body": body({"content": "x", "error": None})})
file_op("read-lines-string", "file_read", path="/a", offset=0, limit=0,
        response={"code": 200, "body": body(
            {"content": "a\nb", "size": 3, "lines": "5"})})
file_op("read-lines-float", "file_read", path="/a", offset=0, limit=0,
        response={"code": 200, "body": body(
            {"content": "a\nb", "size": 3, "lines": 5.5})})
file_op("read-size-string", "file_read", path="/a", offset=0, limit=5,
        response={"code": 200, "body": body(
            {"content": "abcde", "size": "12"})})
file_op("read-truncated-string-flag", "file_read", path="/a", offset=0, limit=5,
        response={"code": 200, "body": body(
            {"content": "abcde", "size": 5, "truncated": "true"})})
file_op("read-truncated-invalid-flag", "file_read", path="/a", offset=0, limit=5,
        response={"code": 200, "body": body(
            {"content": "abcde", "size": 9, "truncated": "maybe"})})
file_op("read-unicode-content", "file_read", path="/a", offset=0, limit=0,
        response={"code": 200, "body": body(
            {"content": "héllo 世界", "size": 13})})
file_op("read-http-500", "file_read", path="/a", offset=0, limit=0,
        response={"code": 500, "body": "boom"})
file_op("read-encrypted-http-500", "file_read", path="/a", offset=0, limit=0,
        api_key=KEY, script_handshake=True,
        response={"code": 500, "body": "boom"})
file_op("read-http-empty-body", "file_read", path="/a", offset=0, limit=0,
        response={"code": 500, "body": ""})
file_op("read-http-long-body", "file_read", path="/a", offset=0, limit=0,
        response={"code": 500, "body": "y" * 300})
file_op("read-unknown-host", "file_read", path="/a", offset=0, limit=0,
        transport_failure="unknown_host", transport_failure_detail="db.internal")
file_op("read-connect", "file_read", path="/a", offset=0, limit=0,
        transport_failure="connect", transport_failure_detail="connection refused")
file_op("read-socket-timeout", "file_read", path="/a", offset=0, limit=0,
        transport_failure="socket_timeout", transport_failure_detail="timeout")
file_op("read-interrupted-io", "file_read", path="/a", offset=0, limit=0,
        transport_failure="interrupted_io", transport_failure_detail="stop")
file_op("read-ssl", "file_read", path="/a", offset=0, limit=0,
        transport_failure="ssl", transport_failure_detail="bad cert")
file_op("read-plain-exception", "file_read", path="/a", offset=0, limit=0,
        transport_failure="plain", transport_failure_detail="kaboom")
file_op("read-encrypted-auth-invalid", "file_read", path="/a", offset=0, limit=0,
        api_key=KEY, response={"code": 200, "body": body({"content": "secret"}),
                               "signature": "invalid"})
file_op("read-encrypted-decrypt-short", "file_read", path="/a", offset=0, limit=0,
        api_key=KEY, response={"code": 200, "body": "unused",
                               "raw_body": "AAAA", "signature": "valid"})
file_op("read-encrypted-success", "file_read", path="/a", offset=0, limit=0,
        api_key=KEY, response={"code": 200,
                               "body": body({"content": "secret", "size": 6})})

# ---- file_write ----------------------------------------------------------------------------------
file_op("write-plain-ok", "file_write", path="/tmp/b.txt", content="data",
        response={"code": 200, "body": "{}"})
file_op("write-plain-error", "file_write", path="/tmp/b.txt", content="data",
        response={"code": 200, "body": body({"error": "permission denied"})})
file_op("write-encrypted-ok", "file_write", path="/tmp/b.txt", content="data",
        api_key=KEY, response={"code": 200, "body": "{}"})

# ---- file_edit -----------------------------------------------------------------------------------
file_op("edit-plain-ok", "file_edit", path="/tmp/c.txt", old_string="foo",
        new_string="bar", replace_all=False,
        response={"code": 200, "body": body({"replacements": 2, "sha256": "abc123"})})
file_op("edit-replace-all", "file_edit", path="/tmp/c.txt", old_string="foo",
        new_string="bar", replace_all=True,
        response={"code": 200, "body": body({"replacements": 2, "sha256": "abc123"})})
file_op("edit-error", "file_edit", path="/tmp/c.txt", old_string="nope",
        new_string="bar", replace_all=False,
        response={"code": 200, "body": body(
            {"error": "old_string not found in file"})})
file_op("edit-error-null", "file_edit", path="/tmp/c.txt", old_string="foo",
        new_string="bar", replace_all=False,
        response={"code": 200, "body": body({"replacements": 1, "error": None})})
file_op("edit-replacements-string", "file_edit", path="/tmp/c.txt",
        old_string="foo", new_string="bar", replace_all=False,
        response={"code": 200, "body": body(
            {"replacements": "3", "sha256": "deadbeef"})})
file_op("edit-missing-fields", "file_edit", path="/tmp/c.txt", old_string="foo",
        new_string="bar", replace_all=False, response={"code": 200, "body": "{}"})

# ---- file_glob -----------------------------------------------------------------------------------
file_op("glob-ok", "file_glob", pattern="*.txt", path="", depth=None,
        response={"code": 200, "body": body(
            {"files": ["/a.txt", "/b.txt"], "truncated": False})})
file_op("glob-with-path-depth", "file_glob", pattern="*.txt", path="/tmp",
        depth=2, response={"code": 200, "body": body({"files": ["/tmp/a.txt"]})})
file_op("glob-depth-negative", "file_glob", pattern="*", path="", depth=-1,
        response={"code": 200, "body": body({"files": []})})
file_op("glob-truncated-invalid", "file_glob", pattern="*", path="", depth=None,
        response={"code": 200, "body": body(
            {"files": ["/x"], "truncated": "yes"})})
file_op("glob-error", "file_glob", pattern="bad[", path="", depth=None,
        response={"code": 200, "body": body({"error": "bad pattern"})})
file_op("glob-not-array", "file_glob", pattern="*", path="", depth=None,
        response={"code": 200, "body": body({"files": "oops"})})

# ---- file_grep -----------------------------------------------------------------------------------
file_op("grep-ok", "file_grep", pattern="hit", path="", glob="",
        response={"code": 200, "body": body(
            {"matches": [{"path": "/a", "line": 3, "content": "hit me"}],
             "truncated": True})})
file_op("grep-with-path-glob", "file_grep", pattern="hit", path="/src",
        glob="*.kt", response={"code": 200, "body": body(
            {"matches": [{"path": "/src/A.kt", "line": 1, "content": "hit"}]})})
file_op("grep-bad-line", "file_grep", pattern="x", path="", glob="",
        response={"code": 200, "body": body(
            {"matches": [{"path": "/a", "line": "abc"}]})})
file_op("grep-truncated-string", "file_grep", pattern="x", path="", glob="",
        response={"code": 200, "body": body(
            {"matches": [], "truncated": "true"})})
file_op("grep-error", "file_grep", pattern="x", path="", glob="",
        response={"code": 200, "body": body({"error": "regex broke"})})

# ---- file_image ----------------------------------------------------------------------------------
file_op("image-ok", "file_image", path="/img.png",
        response={"code": 200, "body": body(
            {"data": "AAA", "mimeType": "image/png", "size": 123})})
file_op("image-error", "file_image", path="/img.png",
        response={"code": 200, "body": body({"error": "not an image"})})
file_op("image-missing-fields", "file_image", path="/img.png",
        response={"code": 200, "body": "{}"})

# ---- jobs ------------------------------------------------------------------------------------------
job("job-start-plain", "job_start", command="ls -la", timeout_ms=5000,
    workdir="", response={"code": 200, "body": body({"job_id": "job-1"})})
job("job-start-workdir", "job_start", command="ls -la", timeout_ms=5000,
    workdir="/home/user", response={"code": 200, "body": body({"job_id": "job-1"})})
job("job-list", "job_list", response={"code": 200, "body": "[]"})
job("job-get", "job_get", job_id="job-9",
    response={"code": 200, "body": body({"state": "running"})})
job("job-stop-timeout", "job_stop", job_id="job-9", call_timeout_ms=5000,
    response={"code": 200, "body": "{}"})
job("job-stop-null-timeout", "job_stop", job_id="job-9",
    response={"code": 200, "body": "{}"})
job("job-ack", "job_ack", job_id="job-9", response={"code": 200, "body": "{}"})
job("job-start-encrypted", "job_start", command="ls", timeout_ms=1000,
    workdir="", api_key=KEY, response={"code": 200, "body": body({"job_id": "job-2"})})
job("job-list-http-error", "job_list",
    response={"code": 500, "body": "busy"})


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.endswith(".json"):
            os.unlink(os.path.join(OUT, old))
    for index, (name, scenario) in enumerate(scenarios, start=1):
        path = os.path.join(OUT, f"{index:03d}-{name}.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(scenario, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(f"wrote {len(scenarios)} scenarios to {OUT}")


if __name__ == "__main__":
    main()
