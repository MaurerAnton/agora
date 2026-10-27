#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the shell-tool-backends stand."""
import base64
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")
scenarios = []

SERVER = "http://conch.local:9"
API_KEY = "secret-api-key"


def add(name, scenario):
    scenarios.append((name, scenario))


def compact(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def sse(event, payload):
    return [f"event: {event}", "data: " + compact(payload)]


def conch_device(name="conch-box", url=SERVER, api_key="", pub=""):
    return {
        "name": name, "type": "conch", "serverUrl": url, "apiKey": api_key,
        "conchPublicKey": pub, "description": "Conch host",
    }


def ssh_device(name="pi", host="10.0.0.5", port=22, user="pi", password="pw", host_key=""):
    return {
        "name": name, "type": "ssh", "sshHost": host, "sshPort": port, "sshUser": user,
        "sshPassword": password, "sshHostKey": host_key, "description": "Lab Pi",
    }


# ---- conch_execute -------------------------------------------------------------------------------
def conch_execute(name, *, op="conch_execute", device=None, command="ls -la", workdir="",
                  timeout_ms=30000, stream=None, **extra):
    scenario = {
        "op": op, "device": device or conch_device(),
        "command": command, "workdir": workdir, "timeout_ms": timeout_ms,
        "stream": stream or {"code": 200, "lines": []},
    }
    scenario.update(extra)
    add(name, scenario)


conch_execute(
    "ce-url-blank", device=conch_device(url=""),
    stream={"code": 200, "lines": sse("result", {"exit_code": 0})},
)
conch_execute(
    "ce-plain-success",
    stream={"code": 200, "lines": sse("line", {"line": "hello", "stream": "stdout"}) +
            sse("result", {"exit_code": 0})},
)
conch_execute(
    "ce-plain-stderr",
    stream={"code": 200, "lines": sse("line", {"line": "out1", "stream": "stdout"}) +
            sse("line", {"line": "out2", "stream": "stdout"}) +
            sse("line", {"line": "err1", "stream": "stderr"}) +
            sse("result", {"exit_code": 0})},
)
conch_execute(
    "ce-warning",
    stream={"code": 200, "lines": sse("line", {"line": "x", "stream": "stdout"}) +
            sse("warning", {"message": "disk almost full"}) +
            sse("result", {"exit_code": 0})},
)
conch_execute(
    "ce-error-terminal",
    stream={"code": 200, "lines": sse("error", {"message": "command not found", "timed_out": True})},
)
conch_execute(
    "ce-encrypted-success", device=conch_device(api_key=API_KEY),
    stream={"code": 200, "lines": [
        "event: line",
        "data: " + compact({"_conch_event": "line", "_conch_sequence": 0,
                            "line": "secret", "stream": "stdout"}),
        "event: result",
        "data: " + compact({"_conch_event": "result", "_conch_sequence": 1, "exit_code": 0}),
    ]},
)
conch_execute(
    "ce-encrypted-warning", device=conch_device(api_key=API_KEY),
    stream={"code": 200, "lines": [
        "event: warning",
        "data: " + compact({"_conch_event": "warning", "_conch_sequence": 0,
                            "message": "quota at 90%"}),
        "event: result",
        "data: " + compact({"_conch_event": "result", "_conch_sequence": 1, "exit_code": 3}),
    ]},
)
conch_execute("ce-http-error", stream={"code": 500, "error_body": "oops"})
conch_execute("ce-http-empty", stream={"code": 500, "error_body": ""})
conch_execute("ce-http-long-error", stream={"code": 502, "error_body": "z" * 300})
conch_execute(
    "ce-stream-timeout",
    stream={"failure": "socket_timeout", "failure_detail": "read timed out"},
)
conch_execute(
    "ce-stream-unknown-host",
    stream={"failure": "unknown_host", "failure_detail": "conch.local"},
)
conch_execute(
    "ce-sse-unsupported-event",
    stream={"code": 200, "lines": ["event: bogus", "data: {}"]},
)
conch_execute(
    "ce-sse-no-terminal",
    stream={"code": 200, "lines": sse("line", {"line": "x", "stream": "stdout"})},
)
conch_execute(
    "ce-fetch-handshake-fails", device=conch_device(api_key=API_KEY),
    handshake_sign_with="other-key",
    stream={"code": 200, "lines": sse("result", {"exit_code": 0})},
)
conch_execute(
    "ce-workdir-body", workdir="/home/pi",
    stream={"code": 200, "lines": sse("result", {"exit_code": 0})},
)
conch_execute(
    "ce-events-deltas", op="conch_execute_events",
    stream={"code": 200, "lines": sse("line", {"line": "hello", "stream": "stdout"}) +
            sse("line", {"line": "world", "stream": "stdout"}) +
            sse("result", {"exit_code": 0})},
)

# ---- conch file passthrough --------------------------------------------------------------------
def conch_file(name, op, **kwargs):
    device = kwargs.pop("device", conch_device())
    response = kwargs.pop("response", {"code": 200, "body": "{}"})
    scenario = {"op": op, "device": device, "response": response}
    scenario.update(kwargs)
    add(name, scenario)


conch_file("cr-passthrough", "conch_file_read", path="/data/a.txt", offset=0, limit=0,
           response={"code": 200, "body": compact(
               {"content": "hi", "size": 2, "lines": 1, "totalLines": 1})})
conch_file("cr-throws", "conch_file_read", path="/data/a.txt", offset=0, limit=0,
           response={"failure": "unknown_host", "failure_detail": "db"})
conch_file("cw-ok", "conch_file_write", path="/data/b.txt", content="payload",
           response={"code": 200, "body": "{}"})
conch_file("cw-error", "conch_file_write", path="/data/b.txt", content="payload",
           response={"code": 200, "body": compact({"error": "permission denied"})})
conch_file("cw-encrypted", "conch_file_write", path="/data/b.txt", content="payload",
           device=conch_device(api_key=API_KEY), response={"code": 200, "body": "{}"})
conch_file("ce-file-edit-passthrough", "conch_file_edit", path="/data/c.txt",
           old_string="foo", new_string="baz", replace_all=False,
           response={"code": 200, "body": compact({"replacements": 1, "sha256": "aa"})})
conch_file("cg-error", "conch_file_glob", pattern="*.txt", path="", depth=None,
           response={"code": 200, "body": compact({"error": "bad pattern"})})
conch_file("cgr-matches", "conch_file_grep", pattern="hit", path="", glob="",
           response={"code": 200, "body": compact(
               {"matches": [{"path": "/a", "line": 3, "content": "hit"}],
                "truncated": False})})

# ---- ssh_execute ---------------------------------------------------------------------------------
PRESENTED = "server-host-key"
PRESENTED_B64 = base64.b64encode(PRESENTED.encode()).decode()


def ssh_execute(name, *, op="ssh_execute", device=None, command="ls -la", workdir="",
                timeout_ms=30000, jsch=None, **extra):
    scenario = {
        "op": op, "device": device or ssh_device(), "command": command,
        "workdir": workdir, "timeout_ms": timeout_ms, "jsch": jsch or {},
    }
    scenario.update(extra)
    add(name, scenario)


ssh_execute(
    "ssh-ok", jsch={"presented_key": PRESENTED, "exit_status": 7,
                    "stdout": "line1\nline2", "stderr": "err1"},
)
ssh_execute(
    "ssh-blank-stderr", jsch={"presented_key": PRESENTED, "exit_status": 0,
                              "stdout": "only-out", "stderr": ""},
)
ssh_execute(
    "ssh-strict-match",
    device=ssh_device(host_key=PRESENTED_B64),
    jsch={"presented_key": PRESENTED, "exit_status": 0, "stdout": "ok"},
)
ssh_execute(
    "ssh-strict-reject",
    device=ssh_device(host_key="pinned-key-AAAAAAAAAAAAAAAAAAAAAA=="),
    jsch={"presented_key": PRESENTED, "strict_rejection": "host key rejected by policy"},
)
ssh_execute(
    "ssh-connect-failure",
    jsch={"connect_failure": "Connection refused"},
)
ssh_execute(
    "ssh-no-host",
    device=ssh_device(host=""),
    jsch={"presented_key": PRESENTED},
)
ssh_execute(
    "ssh-timeout", timeout_ms=500,
    jsch={"presented_key": PRESENTED, "never_close": True, "exit_status": 0},
)
ssh_execute(
    "ssh-workdir-escape", workdir="/home/o'brien",
    jsch={"presented_key": PRESENTED, "exit_status": 0, "stdout": "ran"},
)
ssh_execute(
    "ssh-events", op="ssh_execute_events",
    jsch={"presented_key": PRESENTED, "exit_status": 0, "stdout": "done"},
)
add("ssh-fingerprint-valid", {"op": "ssh_fingerprint", "base64_key": "aGVsbG8="})
add("ssh-fingerprint-invalid", {"op": "ssh_fingerprint", "base64_key": "!!not-base64!!"})

# ---- ssh file ops ---------------------------------------------------------------------------------
def ssh_file(name, op, *, device=None, jsch=None, **kwargs):
    scenario = {"op": op, "device": device or ssh_device(), "jsch": jsch or {}}
    scenario.update(kwargs)
    add(name, scenario)


ssh_file("sr-basic", "ssh_file_read", path="/home/pi/x.txt", offset=0, limit=0,
         jsch={"pwd": "/home/pi",
               "stats": {"/home/pi/x.txt": {"size": 11}},
               "file_bytes": {"/home/pi/x.txt": "Hello World"}})
ssh_file("sr-offset-rewind-read", "ssh_file_read", path="/home/pi/x.txt", offset=6, limit=0,
         jsch={"pwd": "/home/pi",
               "stats": {"/home/pi/x.txt": {"size": 11}},
               "file_bytes": {"/home/pi/x.txt": "Hello World"}})
ssh_file("sr-stat-missing", "ssh_file_read", path="/home/pi/gone.txt", offset=0, limit=500,
         jsch={"pwd": "/home/pi", "stats": {}})
ssh_file("sr-session-failure", "ssh_file_read", path="/home/pi/x.txt", offset=0, limit=0,
         jsch={"connect_failure": "Connection refused"})

ssh_file("sw-parents-exist", "ssh_file_write", path="/home/pi/new.txt", content="data",
         jsch={"stats": {"/home": {"is_dir": True}, "/home/pi": {"is_dir": True}}})
ssh_file("sw-mkdir-parent", "ssh_file_write", path="/home/pi/new/deep.txt", content="data",
         jsch={"stats": {"/home": {"is_dir": True}},
               "mkdir_failures": []})
ssh_file("sw-put-fails", "ssh_file_write", path="/home/pi/new.txt", content="data",
         jsch={"stats": {"/home": {"is_dir": True}, "/home/pi": {"is_dir": True}},
               "put_failures": {"/home/pi/new.txt": "disk full"}})

ssh_file("se-basic", "ssh_file_edit", path="/home/pi/c.txt", old_string="foo",
         new_string="baz", replace_all=False,
         jsch={"stats": {"/home/pi/c.txt": {"size": 7}},
               "file_bytes": {"/home/pi/c.txt": "foo bar"}})
ssh_file("se-not-found", "ssh_file_edit", path="/home/pi/c.txt", old_string="nope",
         new_string="baz", replace_all=False,
         jsch={"stats": {"/home/pi/c.txt": {"size": 7}},
               "file_bytes": {"/home/pi/c.txt": "foo bar"}})
ssh_file("se-concurrent-change", "ssh_file_edit", path="/home/pi/c.txt", old_string="foo",
         new_string="baz", replace_all=False,
         jsch={"stats": {"/home/pi/c.txt": {"size": 7}},
               "file_bytes": {"/home/pi/c.txt": "foo bar"},
               "file_bytes_second": {"/home/pi/c.txt": "CHANGED!"}})

ssh_file("sg-basic", "ssh_file_glob", pattern="*.txt", path="", depth=None,
         jsch={"pwd": "/home/pi",
               "ls": {"/home/pi": [["a.txt", "false"], ["dir", "true"]],
                      "/home/pi/dir": [["b.txt", "false"]]}})
ssh_file("sg-depth-1", "ssh_file_glob", pattern="*.txt", path="", depth=1,
         jsch={"pwd": "/home/pi",
               "ls": {"/home/pi": [["a.txt", "false"], ["dir", "true"]],
                      "/home/pi/dir": [["b.txt", "false"]]}})
ssh_file("sg-subdir-ls-fails", "ssh_file_glob", pattern="*.txt", path="", depth=None,
         jsch={"pwd": "/home/pi",
               "ls": {"/home/pi": [["a.txt", "false"], ["dir", "true"]]}})

ssh_file("sgr-server-side", "ssh_file_grep", pattern="it's", path="", glob="*.kt",
         jsch={"pwd": "/home/pi", "exit_status": 0,
               "stdout": "a.kt:3:hit line\nb.kt:7:12:hit two\njunk\nc.kt:nope:x\n",
               "stderr": ""})
ssh_file("sgr-server-no-match", "ssh_file_grep", pattern="absent", path="", glob="",
         jsch={"pwd": "/home/pi", "exit_status": 1, "stdout": "", "stderr": ""})
ssh_file("sgr-fallback", "ssh_file_grep", pattern="hit", path="", glob="",
         jsch={"pwd": "/home/pi", "exit_status": 2, "stdout": "grep: boom\n", "stderr": "",
               "stats": {"./a.txt": {"size": 11}},
               "file_bytes": {"./a.txt": "hello hit\nno"},
               "ls": {".": [["a.txt", "false"]]}})
ssh_file("sgr-fallback-invalid-regex", "ssh_file_grep", pattern="[", path="", glob="",
         jsch={"pwd": "/home/pi", "exit_status": 2, "stdout": "", "stderr": "",
               "stats": {"./a.txt": {"size": 9}},
               "file_bytes": {"./a.txt": "a [b] c\nx"},
               "ls": {".": [["a.txt", "false"]]}})

# ---- sandbox -------------------------------------------------------------------------------------
def sandbox_execute(name, *, command="ls", workdir="", timeout_ms=30000, sandbox=None):
    add(name, {"op": "sandbox_execute", "command": command, "workdir": workdir,
               "timeout_ms": timeout_ms, "sandbox": sandbox or {}})


sandbox_execute("sbx-not-installed", sandbox={"available": False})
sandbox_execute(
    "sbx-success",
    sandbox={"execute_result": {"stdout": "pkg list", "stderr": "", "exit_code": 0}},
)
sandbox_execute(
    "sbx-warning-and-stderr",
    sandbox={"execute_result": {"stdout": "out", "stderr": "err", "exit_code": 2,
                                "warning": "proot fallback engaged"}},
)
sandbox_execute("sbx-exec-fails", sandbox={"execute_failure": "proot crashed"})

add("sbx-file-read", {"op": "sandbox_file_read", "path": "/mnt/home/a.txt", "offset": 0,
                      "limit": 0, "sandbox": {"file_read_result": {
                          "content": "sandbox data", "lines": 1, "total_lines": 1,
                          "total_bytes": 12, "returned_bytes": 12, "offset": 0,
                          "limit": 1048576, "truncated": False}}})
add("sbx-file-read-throws", {"op": "sandbox_file_read", "path": "/mnt/home/a.txt",
                             "offset": 0, "limit": 500,
                             "sandbox": {"file_read_failure": "rootfs vanished"}})
add("sbx-file-write-error", {"op": "sandbox_file_write", "path": "/mnt/home/b.txt",
                             "content": "x",
                             "sandbox": {"file_write_error": "read-only mount"}})
add("sbx-file-edit", {"op": "sandbox_file_edit", "path": "/mnt/home/c.txt",
                      "old_string": "foo", "new_string": "baz", "replace_all": False,
                      "sandbox": {"file_edit_result": {
                          "replacements": 2, "sha256": "cafe"}}})
add("sbx-file-glob-ok", {"op": "sandbox_file_glob", "pattern": "*.txt", "path": "",
                         "depth": None,
                         "sandbox": {"file_glob_result": {
                             "files": ["/mnt/home/a.txt"], "truncated": False}}})
add("sbx-file-glob-failure", {"op": "sandbox_file_glob", "pattern": "[", "path": "",
                              "depth": None,
                              "sandbox": {"file_glob_failure": "glob blew up"}})
add("sbx-file-grep-ok", {"op": "sandbox_file_grep", "pattern": "x", "path": "", "glob": "",
                         "sandbox": {"file_grep_result": {
                             "matches": [{"path": "/mnt/home/a.txt", "line": 1,
                                          "content": "xyz"}],
                             "truncated": True}}})
add("sbx-file-grep-failure", {"op": "sandbox_file_grep", "pattern": "x", "path": "",
                              "glob": "",
                              "sandbox": {"file_grep_failure": "grep unavailable"}})


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
