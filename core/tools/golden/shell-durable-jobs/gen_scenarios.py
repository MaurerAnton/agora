#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the shell-durable-jobs stand."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")
scenarios = []

CONCH1 = [{"name": "home", "type": "conch"}]
CONCH2 = [{"name": "alpha", "type": "conch"}, {"name": "beta", "type": "conch"}]
SSH1 = [{"name": "rasp", "type": "ssh"}]
MIXED = [{"name": "home", "type": "conch"}, {"name": "rasp", "type": "ssh"}]
UNTITLED = [{"name": "", "type": "conch"}]


def payload(obj):
    """Compact JSON text used as a scripted Conch response."""
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def add(name, scenario):
    scenarios.append((name, scenario))


# ---- resolve_conch_device ------------------------------------------------------------------------
add("resolve-match-case", {"op": "resolve_conch_device", "server": "HOME", "devices": CONCH1})
add("resolve-blank-single", {"op": "resolve_conch_device", "server": "", "devices": CONCH1})
add("resolve-blank-multi", {"op": "resolve_conch_device", "server": "", "devices": CONCH2})
add("resolve-named", {"op": "resolve_conch_device", "server": "beta", "devices": CONCH2})
add("resolve-ssh-excluded", {"op": "resolve_conch_device", "server": "rasp", "devices": MIXED})
add("resolve-blank-ssh-only", {"op": "resolve_conch_device", "server": "", "devices": SSH1})
add("resolve-unknown-named", {"op": "resolve_conch_device", "server": "ghost", "devices": CONCH1})

# ---- conch_not_found -----------------------------------------------------------------------------
add("notfound-no-config", {"op": "conch_not_found", "server": "", "devices": []})
add("notfound-unknown-named", {"op": "conch_not_found", "server": "ghost", "devices": CONCH2})
add("notfound-blank-multi", {"op": "conch_not_found", "server": "", "devices": CONCH2})
add("notfound-blank-single", {"op": "conch_not_found", "server": "", "devices": CONCH1})
add("notfound-ssh-only", {"op": "conch_not_found", "server": "", "devices": SSH1})
add("notfound-untitled", {"op": "conch_not_found", "server": "x", "devices": UNTITLED})
add("notfound-untitled-list", {
    "op": "conch_not_found", "server": "",
    "devices": [{"name": " ", "type": "conch"}, {"name": "b", "type": "conch"}],
})

# ---- is_terminal_payload -------------------------------------------------------------------------
TERMINAL = [
    ("succeeded", '{"state":"succeeded"}'),
    ("failed-upper", '{"state":"FAILED"}'),
    ("stopped", '{"state":"stopped"}'),
    ("interrupted", '{"state":"interrupted"}'),
    ("running", '{"state":"running"}'),
    ("stopping", '{"state":"stopping"}'),
    ("settling", '{"state":"settling"}'),
    ("error-present", '{"error":"job not found"}'),
    ("error-null", '{"error":null}'),
    ("error-with-running-state", '{"state":"running","error":"boom"}'),
    ("state-null", '{"state":null}'),
    ("state-number", '{"state":5}'),
    ("state-blank", '{"state":""}'),
    ("state-object", '{"state":{"code":1},"error":"x"}'),
    ("empty-object", "{}"),
    ("unparseable", "{broken"),
    ("blank", "   "),
    ("array", "[1,2]"),
    ("json-string", '"succeeded"'),
    ("error-object-value", '{"error":{"code":500}}'),
    ("duplicate-state", '{"state":"running","state":"succeeded"}'),
    ("result-only", '{"result":"x"}'),
    ("stopped-with-error", '{"state":"stopped","error":"killed"}'),
    ("bare-word", "succeeded"),
]
for suffix, raw in TERMINAL:
    add("terminal-" + suffix, {"op": "is_terminal_payload", "raw": raw})

# ---- max_wait_ms ---------------------------------------------------------------------------------
for suffix, ms in [
    ("60s", 60000), ("6s", 6000), ("exact-5000", 5000), ("5001", 5001),
    ("7s", 7000), ("zero", 0), ("int-truncation", 5000000000),
]:
    add("maxwait-" + suffix, {"op": "max_wait_ms", "tool_timeout_ms": ms})

# ---- output_snapshot -----------------------------------------------------------------------------
SNAPSHOTS = [
    ("basic", '{"output":"hello","output_bytes":5}'),
    ("fallback-truncated", '{"output":"hi","output_bytes":10}'),
    ("explicit-false", '{"output":"hi","output_bytes":10,"truncated":false}'),
    ("explicit-true", '{"output":"hello","output_bytes":1,"truncated":true}'),
    ("string-bytes", '{"output":"hi","output_bytes":"5"}'),
    ("negative", '{"output":"hi","output_bytes":-1}'),
    ("float", '{"output":"hi","output_bytes":2.5}'),
    ("overflow", '{"output":"hi","output_bytes":99999999999999999999}'),
    ("missing-output", '{"output_bytes":5}'),
    ("number-output", '{"output":7,"output_bytes":5}'),
    ("truncated-string-value", '{"output":"hi","output_bytes":10,"truncated":"true"}'),
    ("missing-bytes", '{"output":"hi"}'),
    ("bytes-object", '{"output":"hi","output_bytes":{"n":1}}'),
    ("unicode", '{"output":"héllo 世界","output_bytes":13}'),
    ("astral", '{"output":"\U0001F600","output_bytes":4}'),
    ("astral-truncated", '{"output":"\U0001F600","output_bytes":3}'),
    ("truncated-null", '{"output":"hi","output_bytes":10,"truncated":null}'),
    ("truncated-false-string", '{"output":"hello","output_bytes":6,"truncated":"false"}'),
]
for suffix, raw in SNAPSHOTS:
    add("snapshot-" + suffix, {"op": "output_snapshot", "raw": raw})

# ---- output_cursor -------------------------------------------------------------------------------
add("cursor-initial", {
    "op": "output_cursor",
    "payloads": ['{"output":"abc","output_bytes":3}'],
})
add("cursor-append", {
    "op": "output_cursor",
    "payloads": ['{"output":"abc","output_bytes":3}', '{"output":"abcdef","output_bytes":6}'],
})
add("cursor-gap", {
    "op": "output_cursor",
    "payloads": ['{"output":"def","output_bytes":6}'],
})
add("cursor-duplicate", {
    "op": "output_cursor",
    "payloads": ['{"output":"abc","output_bytes":3}', '{"output":"abc","output_bytes":3}'],
})
add("cursor-backwards", {
    "op": "output_cursor",
    "payloads": ['{"output":"abcdefgh","output_bytes":8}', '{"output":"ab","output_bytes":2}'],
})
add("cursor-missing-fields", {
    "op": "output_cursor",
    "payloads": ['{"output":"abc"}', "not json", '{"output_bytes":3}',
                 '{"output":"","output_bytes":0}'],
})
add("cursor-window-shift", {
    "op": "output_cursor",
    "payloads": ['{"output":"abcdef","output_bytes":6}', '{"output":"fgh","output_bytes":9}'],
})
add("cursor-shared-tail", {
    "op": "output_cursor",
    "payloads": ['{"output":"abcdefgh","output_bytes":10}',
                 '{"output":"cdefghij","output_bytes":10}'],
})
add("cursor-evict-reemit", {
    "op": "output_cursor",
    "payloads": ['{"output":"abcdef","output_bytes":6}',
                 '{"output":"defghij","output_bytes":10}'],
})
add("cursor-zero-bytes", {
    "op": "output_cursor",
    "payloads": ['{"output":"","output_bytes":0}'],
})

# ---- wait_for_job_polling ------------------------------------------------------------------------


def polling(name, *, timeout_ms=30000, ceiling_ms=55000, clamped_from=None, server="srv",
            job_id="job-1", start_clock=1000000, cancel_after_delay=None, polls=(),
            poll_repeat=None):
    scenario = {
        "op": "wait_for_job_polling",
        "job_id": job_id,
        "server": server,
        "timeout_ms": timeout_ms,
        "clamped_from": clamped_from,
        "ceiling_ms": ceiling_ms,
        "start_clock": start_clock,
        "polls": list(polls),
        "cancel_after_delay": cancel_after_delay,
    }
    if poll_repeat is not None:
        scenario["poll_repeat"] = poll_repeat
    add(name, scenario)


running = {"ok": payload({"state": "running"})}
polling("polling-terminal-first",
        polls=[{"ok": payload({"state": "succeeded", "exit_code": 0})}])
polling("polling-transient-terminal",
        polls=[{"throw": "connection reset"}, {"ok": payload({"state": "failed", "exit_code": 1})}])
polling("polling-snapshot-publish", polls=[
    {"ok": payload({"state": "running", "output": "line1\n", "output_bytes": 6})},
    {"ok": payload({"state": "succeeded", "output": "line1\nline2", "output_bytes": 12})},
])
polling("polling-snapshot-dedupe", polls=[
    {"ok": payload({"state": "running", "output": "same", "output_bytes": 4})},
    {"ok": payload({"state": "running", "output": "same", "output_bytes": 4})},
    {"ok": payload({"state": "succeeded", "output": "same", "output_bytes": 4})},
])
polling("polling-timeout", timeout_ms=1000, polls=[running], poll_repeat=running)
polling("polling-timeout-with-snapshot", timeout_ms=1000,
        polls=[{"ok": payload({"state": "running", "output": "tick", "output_bytes": 4})}],
        poll_repeat={"ok": payload({"state": "running", "output": "tick", "output_bytes": 4})})
polling("polling-timeout-clamp-note", timeout_ms=1000, ceiling_ms=1000, clamped_from=5000,
        polls=[running], poll_repeat=running)
polling("polling-poll-failed-5", polls=[{"throw": "socket closed"}] * 5)
polling("polling-poll-failed-with-snapshot", polls=[
    {"ok": payload({"state": "running", "output": "head", "output_bytes": 4})},
    {"throw": "socket closed"}, {"throw": "socket closed"}, {"throw": "socket closed"},
    {"throw": "socket closed"}, {"throw": "socket closed"},
])
polling("polling-cancel-during-poll", polls=[
    {"cancel": payload({"state": "running", "output": "x", "output_bytes": 1})},
])
polling("polling-cancel-then-terminal", polls=[
    {"cancel": payload({"state": "succeeded", "job_id": "job-1"})},
])
polling("polling-cancel-then-timeout", timeout_ms=0,
        polls=[{"cancel": payload({"state": "running"})}])
polling("polling-cancel-after-delay", cancel_after_delay=1, polls=[running], poll_repeat=running)
polling("polling-cancel-fail", polls=[{"cancel_throw": "generation stopped"}])
polling("polling-exhausted", polls=[])
polling("polling-error-terminal", polls=[{"ok": payload({"error": "job not found"})}])
polling("polling-unicode-snapshot-timeout", timeout_ms=0, polls=[
    {"ok": payload({"state": "running", "output": "héllo 世界", "output_bytes": 13})},
])

# ---- wait_for_job (public path, real clock; waited_ms is masked) ---------------------------------


def wait_public(name, arguments, devices, tool_timeout_ms=60000, polls=(), poll_repeat=None):
    scenario = {
        "op": "wait_for_job",
        "arguments": arguments,
        "devices": devices,
        "tool_timeout_ms": tool_timeout_ms,
        "polls": list(polls),
    }
    if poll_repeat is not None:
        scenario["poll_repeat"] = poll_repeat
    add(name, scenario)


wait_public("wait-missing-job-id", "{}", CONCH1)
wait_public("wait-missing-timeout", payload({"job_id": "job-1"}), CONCH1)
wait_public("wait-bad-timeout",
            payload({"job_id": "job-1", "timeout_ms": "soon", "server": "home"}), CONCH1)
wait_public("wait-unknown-server",
            payload({"job_id": "job-1", "timeout_ms": "5000", "server": "ghost"}), CONCH2)
wait_public("wait-terminal-first",
            payload({"job_id": "job-1", "timeout_ms": "30000", "server": ""}), CONCH1,
            polls=[{"ok": payload({"state": "succeeded", "exit_code": 0})}])
wait_public("wait-transient-terminal",
            payload({"job_id": "job-1", "timeout_ms": "30000", "server": ""}), CONCH1,
            polls=[{"throw": "connection reset"}, {"ok": payload({"state": "failed"})}])
_running_output = {"ok": payload({"state": "running", "output": "x", "output_bytes": 1})}
wait_public("wait-timeout-clamp",
            payload({"job_id": "job-1", "timeout_ms": "5000", "server": ""}), CONCH1,
            tool_timeout_ms=6000, polls=[_running_output], poll_repeat=_running_output)

# ---- durable_foreground (public path, real clock; waited_ms is masked) ---------------------------


def durable(name, *, server="home", command="echo hi", workdir="", wait_ms=60000,
            devices=CONCH1, start=None, polls=(), poll_repeat=None, stop=None):
    scenario = {
        "op": "durable_foreground",
        "server": server,
        "devices": devices,
        "command": command,
        "workdir": workdir,
        "wait_ms": wait_ms,
        "polls": list(polls),
    }
    if start is not None:
        scenario["start"] = start
    if poll_repeat is not None:
        scenario["poll_repeat"] = poll_repeat
    if stop is not None:
        scenario["stop"] = stop
    add(name, scenario)


_start_ok = {"ok": payload({"job_id": "job-1"})}
_succeeded = {"ok": payload({"state": "succeeded", "exit_code": 0})}
durable("durable-terminal-first", start=_start_ok, polls=[_succeeded])
durable("durable-server-case", server="HOME", start=_start_ok, polls=[_succeeded])
durable("durable-start-throw", start={"throw": "unreachable host"})
durable("durable-start-error-passthrough", start={"ok": '{"error": "denied"}'})
durable("durable-start-unparseable", start={"ok": "not-json"})
durable("durable-start-no-job-id", start={"ok": payload({"status": "queued"})})
durable("durable-start-error-null", start={"ok": payload({"error": None})})
durable("durable-poll-failed-5", start=_start_ok,
        polls=[{"throw": "socket closed"}] * 5)
durable("durable-transient-terminal", start=_start_ok,
        polls=[{"throw": "connection reset"}, _succeeded])
_expiry_running = {"ok": payload({"state": "running", "output": "x", "output_bytes": 1})}
durable("durable-wait-expiry", wait_ms=0, start=_start_ok, polls=[_expiry_running])
durable("durable-gap-outputs", wait_ms=0, start=_start_ok,
        polls=[{"ok": payload({"output": "def", "output_bytes": 6})}])
durable("durable-cancel-fail", start=_start_ok,
        polls=[{"cancel_throw": "stop requested"}])
durable("durable-cancel-expiry", wait_ms=0, start=_start_ok,
        polls=[{"cancel": payload({"state": "running"})}])
durable("durable-cancel-then-delay", start=_start_ok,
        polls=[{"cancel": payload({"state": "running"})}])
durable("durable-workdir-command", command="cd /tmp && echo héllo", workdir="/tmp",
        start=_start_ok, polls=[_succeeded])

# ---- list_jobs -----------------------------------------------------------------------------------


def list_jobs(name, arguments, devices, list_step=None):
    scenario = {"op": "list_jobs", "arguments": arguments, "devices": devices}
    if list_step is not None:
        scenario["list"] = list_step
    add(name, scenario)


list_jobs("list-success", payload({"server": "home"}), CONCH1,
          list_step={"ok": payload([{"job_id": "job-1", "state": "running"}])})
list_jobs("list-unknown-server", payload({"server": "ghost"}), CONCH1)
list_jobs("list-throw", payload({"server": "home"}), CONCH1,
          list_step={"throw": "maintenance window"})
list_jobs("list-blank-single", "{}", CONCH1, list_step={"ok": "[]"})
list_jobs("list-invalid-args", "not json", CONCH1, list_step={"ok": "[]"})
list_jobs("list-two-devices-blank", "{}", CONCH2)

# ---- get_jobs ------------------------------------------------------------------------------------


def get_jobs(name, arguments, devices, polls=(), poll_repeat=None):
    scenario = {"op": "get_jobs", "arguments": arguments, "devices": devices,
                "polls": list(polls)}
    if poll_repeat is not None:
        scenario["poll_repeat"] = poll_repeat
    add(name, scenario)


get_jobs("get-blank-job-id", payload({"server": "home"}), CONCH1)
get_jobs("get-success", payload({"job_id": "job-1", "server": "home"}), CONCH1,
         polls=[{"ok": payload({"state": "succeeded", "exit_code": 0})}])
get_jobs("get-unknown-server", payload({"job_id": "job-1", "server": "ghost"}), CONCH1)
get_jobs("get-throw", payload({"job_id": "job-1", "server": "home"}), CONCH1,
         polls=[{"throw": "gone"}])
get_jobs("get-null-job-id", '{"job_id":null,"server":""}', CONCH1,
         polls=[{"ok": payload({"state": "running"})}])
get_jobs("get-case-insensitive-server", payload({"job_id": "job-1", "server": "HOME"}), CONCH1,
         polls=[{"throw": "gone"}])

# ---- terminal_acks -------------------------------------------------------------------------------


def ack(name, tool_name, arguments, result):
    add(name, {
        "op": "terminal_acks",
        "calls": [{"tool_name": tool_name, "arguments": arguments, "result": result}],
    })


SHELL = "execute_shell_command"
ack("acks-envelope-state", SHELL, payload({"server": "home"}),
    payload({"job_id": "job-1", "state": "succeeded", "server": "home"}))
ack("acks-nested-state", SHELL, payload({"server": "home"}),
    payload({"result": {"state": "failed"}, "job_id": "job-2", "server": "alpha"}))
ack("acks-nested-state-priority", SHELL, payload({"server": "srv"}),
    payload({"job_id": "job-3", "state": "running", "result": {"state": "stopped"}}))
ack("acks-non-terminal", SHELL, payload({"server": "home"}),
    payload({"job_id": "job-4", "state": "running"}))
ack("acks-missing-state", SHELL, payload({"server": "home"}), payload({"job_id": "job-5"}))
ack("acks-wrong-tool", "file_read", payload({"server": "home"}),
    payload({"job_id": "job-6", "state": "succeeded"}))
ack("acks-missing-job-id", SHELL, payload({"server": "home"}),
    payload({"state": "succeeded"}))
ack("acks-server-arguments-fallback", SHELL, payload({"server": "fromargs"}),
    payload({"job_id": "job-7", "state": "stopped"}))
ack("acks-server-nested-fallback", SHELL, payload({"server": "srv"}),
    payload({"job_id": "job-8", "state": "stopped", "result": {"server": "fromnested"}}))
ack("acks-server-blank-default", SHELL, payload({"server": "home"}),
    payload({"job_id": "job-9", "state": "stopped"}))
ack("acks-arguments-not-object", SHELL, "nope",
    payload({"job_id": "job-10", "state": "stopped"}))
ack("acks-case-insensitive-state", SHELL, payload({"server": "home"}),
    payload({"job_id": "job-11", "state": "SUCCEEDED"}))
ack("acks-state-from-envelope-when-nested-blank", SHELL, payload({"server": "home"}),
    payload({"result": {"state": ""}, "state": "interrupted", "job_id": "job-12"}))
ack("acks-blank-job-id-falls-to-nested", SHELL, payload({"server": "s"}),
    payload({"result": {"job_id": "job-n"}, "job_id": "", "state": "failed"}))
ack("acks-job-id-null-primitive", SHELL, payload({"server": "home"}),
    payload({"job_id": None, "state": "succeeded"}))
ack("acks-result-not-object", SHELL, payload({"server": "home"}), '"just a string"')
ack("acks-result-not-json", SHELL, payload({"server": "home"}), "garbage")
ack("acks-state-number", SHELL, payload({"server": "home"}),
    payload({"job_id": "job-13", "state": 5}))
ack("acks-get-shell-job-tool", "get_shell_job", payload({"server": "home"}),
    payload({"job_id": "job-14", "state": "failed", "server": "home"}))

add("acks-dedupe", {
    "op": "terminal_acks",
    "calls": [
        {"tool_name": SHELL, "arguments": payload({"server": "Home"}),
         "result": payload({"job_id": "job-15", "state": "stopped", "server": "Home"})},
        {"tool_name": SHELL, "arguments": payload({"server": "home"}),
         "result": payload({"job_id": "job-15", "state": "stopped", "server": "home"})},
    ],
})
add("acks-dedupe-different-job", {
    "op": "terminal_acks",
    "calls": [
        {"tool_name": SHELL, "arguments": payload({"server": "home"}),
         "result": payload({"job_id": "job-16", "state": "stopped", "server": "home"})},
        {"tool_name": SHELL, "arguments": payload({"server": "home"}),
         "result": payload({"job_id": "job-17", "state": "stopped", "server": "home"})},
    ],
})
add("acks-non-shell-ignored", {
    "op": "terminal_acks",
    "calls": [
        {"tool_name": "file_read", "arguments": "{}", "result": "{}"},
        {"tool_name": SHELL, "arguments": payload({"server": "home"}),
         "result": payload({"job_id": "job-18", "state": "interrupted"})},
    ],
})


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
