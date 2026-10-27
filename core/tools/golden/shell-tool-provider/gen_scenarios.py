#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the shell-tool-provider stand.

Every scenario drives tool/ShellToolProvider.kt through the pinned backends:
  * execute / events  — one provider call with a scripted Conch/SSH/Sandbox target;
  * acknowledge       — acknowledgeCommittedJobs over committed ToolCallData rows;
  * definitions       — the advertised tool set for a GenerationContext.
Transport scripts are recorded separately so the trace pins how the provider reached
the backends (URLs, bodies, headers, stream limits, SFTP events, sandbox calls).
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")
os.makedirs(OUT, exist_ok=True)

scenarios = []

SERVER = "http://conch.local:9"


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


def ctx(devices, sandbox=False, shared=False, shell_enabled=True, timeout=60000, conversation="conv-1"):
    return {
        "conversationId": conversation,
        "shellEnabled": shell_enabled,
        "shellDevices": devices,
        "sandboxEnabled": sandbox,
        "sandboxSharedStorageEnabled": shared,
        "toolTimeoutMs": timeout,
    }


def call(name, arguments):
    return {"name": name, "arguments": compact(arguments) if not isinstance(arguments, str) else arguments}


def execute(name, arguments, context, **extra):
    scenario = {"op": "execute", "context": context, "tool_call": call(name, arguments)}
    scenario.update(extra)
    return scenario


def events(name, arguments, context, **extra):
    scenario = {"op": "events", "context": context, "tool_call": call(name, arguments)}
    scenario.update(extra)
    return scenario


def conch_response(body, code=200, **extra):
    response = {"code": code, "body": body}
    response.update(extra)
    return response


# =====================================================================================
# list_shells
# =====================================================================================

add("001-list-empty", execute("list_shells", {}, ctx([], sandbox=False)))
add("002-list-sandbox-only", execute("list_shells", {}, ctx([], sandbox=True),
                                    sandbox={"available": True}))
add("003-list-sandbox-unavailable", execute("list_shells", {}, ctx([], sandbox=True),
                                            sandbox={"available": False}))
add("004-list-one-conch", execute("list_shells", {}, ctx([conch_device()])))
add("005-list-one-ssh", execute("list_shells", {}, ctx([ssh_device()])))
add("006-list-multi", execute("list_shells", {},
                              ctx([conch_device("c1"), conch_device("c2"), ssh_device("pi1")])))
add("007-list-blank-name", execute("list_shells", {},
                                   ctx([{"name": "", "type": "conch", "serverUrl": SERVER}],
                                       sandbox=True)))

# =====================================================================================
# definitions
# =====================================================================================

add("008-defs-disabled", {"op": "definitions", "context": ctx([conch_device()], shell_enabled=False)})
add("009-defs-no-targets", {"op": "definitions", "context": ctx([], sandbox=False)})
add("010-defs-sandbox-only", {"op": "definitions", "context": ctx([], sandbox=True)})
add("011-defs-one-conch", {"op": "definitions", "context": ctx([conch_device()])})
add("012-defs-multi", {"op": "definitions",
                       "context": ctx([conch_device("c1"), conch_device("c2")])})
add("013-defs-mixed", {"op": "definitions",
                       "context": ctx([conch_device("c1"), ssh_device("pi")], sandbox=True)})

# =====================================================================================
# execute_shell_command — validation
# =====================================================================================

add("020-exec-no-command", execute("execute_shell_command", {"timeout_ms": 5000},
                                   ctx([conch_device()])))
add("021-exec-no-timeout", execute("execute_shell_command", {"command": "ls"},
                                   ctx([conch_device()])))
add("022-exec-bad-timeout", execute("execute_shell_command",
                                    {"command": "ls", "timeout_ms": "soon"},
                                    ctx([conch_device()])))
add("023-exec-timeout-clamp-low", execute("execute_shell_command",
                                          {"command": "ls", "timeout_ms": 500},
                                          ctx([conch_device()]),
                                          conch_responses=[
                                              conch_response('{"job_id":"job-1"}'),
                                              conch_response(
                                                  '{"state":"succeeded","output":"",'
                                                  '"output_bytes":0}'),
                                          ]))
add("024-exec-command-too-long", execute("execute_shell_command",
                                         {"command": "x" * 65537, "timeout_ms": 5000},
                                         ctx([conch_device()])))
add("025-exec-workdir-too-long", execute("execute_shell_command",
                                         {"command": "ls", "workdir": "x" * 32769,
                                          "timeout_ms": 5000},
                                         ctx([conch_device()])))
add("026-exec-server-not-found", execute("execute_shell_command",
                                         {"command": "ls", "server": "ghost", "timeout_ms": 5000},
                                         ctx([conch_device()])))
add("027-exec-server-ambiguous", execute("execute_shell_command",
                                         {"command": "ls", "timeout_ms": 5000},
                                         ctx([conch_device("c1"), conch_device("c2")])))
add("028-exec-sandbox-not-installed", execute("execute_shell_command",
                                              {"command": "ls", "timeout_ms": 5000},
                                              ctx([], sandbox=True),
                                              sandbox={"available": False}))
add("029-exec-sandbox-disabled-server", execute("execute_shell_command",
                                                {"command": "ls", "server": "Local Sandbox",
                                                 "timeout_ms": 5000},
                                                ctx([], sandbox=False)))

# =====================================================================================
# execute_shell_command — Conch (durable foreground)
# =====================================================================================

add("030-exec-conch-output-deltas", execute("execute_shell_command",
                                            {"command": "ls", "timeout_ms": 5000},
                                            ctx([conch_device()]),
                                            conch_responses=[
                                                conch_response('{"job_id":"job-1"}'),
                                                conch_response(
                                                    '{"state":"running","output":"bin\\n",'
                                                    '"output_bytes":4}'),
                                                conch_response(
                                                    '{"state":"succeeded","output":"bin\\netc\\n",'
                                                    '"output_bytes":8}'),
                                            ]))
add("031-exec-conch-encrypted", execute("execute_shell_command",
                                       {"command": "ls", "timeout_ms": 5000},
                                       ctx([conch_device(api_key="secret-key")]),
                                       conch_api_key="secret-key",
                                       conch_responses=[
                                           conch_response('{"job_id":"job-1"}'),
                                           conch_response(
                                               '{"state":"succeeded","output":"bin\\n",'
                                               '"output_bytes":4}'),
                                       ]))
add("032-exec-conch-warning", execute("execute_shell_command",
                                      {"command": "ls", "timeout_ms": 5000},
                                      ctx([conch_device()]),
                                      conch_responses=[
                                          conch_response('{"job_id":"job-1"}'),
                                          conch_response(
                                              '{"state":"succeeded","output":"out\\n",'
                                              '"output_bytes":4,"warning":"disk 90%"}'),
                                      ]))
add("033-exec-conch-failed-state", execute("execute_shell_command",
                                           {"command": "fail", "timeout_ms": 5000},
                                           ctx([conch_device()]),
                                           conch_responses=[
                                               conch_response('{"job_id":"job-1"}'),
                                               conch_response(
                                                   '{"state":"failed","error":"command not found"}'),
                                           ]))
add("034-exec-conch-start-non-json", execute("execute_shell_command",
                                             {"command": "ls", "timeout_ms": 5000},
                                             ctx([conch_device()]),
                                             conch_responses=[conch_response("not json")]))
add("035-exec-conch-start-error-object", execute("execute_shell_command",
                                                 {"command": "ls", "timeout_ms": 5000},
                                                 ctx([conch_device()]),
                                                 conch_responses=[conch_response(
                                                     '{"error":"quota exceeded"}')]))
add("036-exec-conch-handshake-failure", execute("execute_shell_command",
                                                {"command": "ls", "timeout_ms": 5000},
                                                ctx([conch_device(api_key="wrong-key")]),
                                                conch_api_key="wrong-key",
                                                conch_responses=[conch_response(
                                                    "{}", code=500)]))
add("037-exec-conch-workdir", execute("execute_shell_command",
                                      {"command": "pwd", "workdir": "/srv/app",
                                       "timeout_ms": 5000},
                                      ctx([conch_device()]),
                                      conch_responses=[
                                          conch_response('{"job_id":"job-1"}'),
                                          conch_response(
                                              '{"state":"succeeded","output":"/srv/app\\n",'
                                              '"output_bytes":9}'),
                                      ]))
add("038-exec-conch-truncated", execute("execute_shell_command",
                                        {"command": "cat big", "timeout_ms": 5000},
                                        ctx([conch_device()]),
                                        conch_responses=[
                                            conch_response('{"job_id":"job-1"}'),
                                            conch_response(
                                                '{"state":"succeeded","output":"partial",'
                                                '"output_bytes":100,"truncated":true}'),
                                        ]))
add("039-exec-conch-output-gap", execute("execute_shell_command",
                                         {"command": "cat big", "timeout_ms": 5000},
                                         ctx([conch_device()]),
                                         conch_responses=[
                                             conch_response('{"job_id":"job-1"}'),
                                             conch_response(
                                                 '{"state":"running","output":"tail",'
                                                 '"output_bytes":100}'),
                                             conch_response(
                                                 '{"state":"succeeded","output":"tail",'
                                                 '"output_bytes":100}'),
                                         ]))
add("044-exec-conch-poll-failure", execute("execute_shell_command",
                                           {"command": "ls", "timeout_ms": 5000},
                                           ctx([conch_device()]),
                                           conch_responses=[
                                               conch_response('{"job_id":"job-1"}'),
                                           ] + [conch_response("boom", code=500)] * 5))
add("049-exec-conch-wait-expiry", execute("execute_shell_command",
                                          {"command": "sleep 5", "timeout_ms": 1000},
                                          ctx([conch_device()]),
                                          conch_responses=[
                                              conch_response('{"job_id":"job-1"}'),
                                          ] + [conch_response(
                                              '{"state":"running","output":"",'
                                              '"output_bytes":0}')] * 4))

# =====================================================================================
# execute_shell_command — background jobs
# =====================================================================================

add("040-exec-background", execute("execute_shell_command",
                                   {"command": "sleep 10", "timeout_ms": 60000,
                                    "background": True},
                                   ctx([conch_device()]),
                                   conch_responses=[conch_response(
                                       '{"job_id":"job-1","state":"running"}')]))
add("041-exec-background-denied", execute("execute_shell_command",
                                          {"command": "sleep 10", "timeout_ms": 60000,
                                           "background": True},
                                          ctx([conch_device()]),
                                          confirm_allowed=False))
add("042-exec-background-ssh", execute("execute_shell_command",
                                       {"command": "sleep 10", "timeout_ms": 60000,
                                        "background": True},
                                       ctx([ssh_device()])))
add("043-exec-background-failure", execute("execute_shell_command",
                                           {"command": "sleep 10", "timeout_ms": 60000,
                                            "background": True},
                                           ctx([conch_device()]),
                                           conch_responses=[conch_response(
                                               "boom", code=500)]))

# =====================================================================================
# execute_shell_command — durable foreground (terminal on first poll)
# =====================================================================================

add("045-exec-durable-terminal", execute("execute_shell_command",
                                         {"command": "echo hi", "timeout_ms": 5000},
                                         ctx([conch_device()]),
                                         conch_responses=[
                                             conch_response('{"job_id":"job-1"}'),
                                             conch_response(
                                                 '{"state":"succeeded","output":"hi\\n","output_bytes":3}'),
                                         ]))
add("046-exec-durable-start-error", execute("execute_shell_command",
                                            {"command": "echo hi", "timeout_ms": 5000},
                                            ctx([conch_device()]),
                                            conch_responses=[conch_response("start failed",
                                                                            code=500)]))
add("047-exec-durable-no-job-id", execute("execute_shell_command",
                                          {"command": "echo hi", "timeout_ms": 5000},
                                          ctx([conch_device()]),
                                          conch_responses=[conch_response('{"ok":true}')]))

# =====================================================================================
# execute_shell_command — SSH / Sandbox
# =====================================================================================

add("050-exec-ssh", execute("execute_shell_command",
                            {"command": "ls", "timeout_ms": 5000},
                            ctx([ssh_device()]),
                            jsch={"presented_key": "server-host-key", "stdout": "bin\netc\n",
                                  "exit_status": 0}))
add("051-exec-ssh-stderr", execute("execute_shell_command",
                                   {"command": "ls /bad", "timeout_ms": 5000},
                                   ctx([ssh_device()]),
                                   jsch={"presented_key": "server-host-key", "stderr":
                                         "No such file", "exit_status": 1}))
add("052-exec-ssh-no-host", execute("execute_shell_command",
                                    {"command": "ls", "timeout_ms": 5000},
                                    ctx([ssh_device(host="")])))
add("053-exec-ssh-connect-failure", execute("execute_shell_command",
                                            {"command": "ls", "timeout_ms": 5000},
                                            ctx([ssh_device()]),
                                            jsch={"connect_failure": "connection refused"}))
add("054-exec-sandbox", execute("execute_shell_command",
                                {"command": "ls", "timeout_ms": 5000},
                                ctx([], sandbox=True),
                                sandbox={"execute_result": {"stdout": "bin\netc\n",
                                                            "exit_code": 0}}))
add("055-exec-sandbox-warning", execute("execute_shell_command",
                                        {"command": "ls", "timeout_ms": 5000},
                                        ctx([], sandbox=True),
                                        sandbox={"execute_result": {"stdout": "bin\n",
                                                                    "stderr": "warn",
                                                                    "exit_code": 0,
                                                                    "warning": "disk low"}}))
add("056-exec-sandbox-failure", execute("execute_shell_command",
                                        {"command": "ls", "timeout_ms": 5000},
                                        ctx([], sandbox=True),
                                        sandbox={"execute_failure": "proot crashed"}))
add("057-exec-sandbox-shared-confirm", execute("execute_shell_command",
                                               {"command": "rm -rf build",
                                                "timeout_ms": 5000},
                                               ctx([], sandbox=True, shared=True),
                                               sandbox={"execute_result": {"stdout": "",
                                                                           "exit_code": 0}}))
add("058-exec-sandbox-shared-denied", execute("execute_shell_command",
                                              {"command": "rm -rf build",
                                               "timeout_ms": 5000},
                                              ctx([], sandbox=True, shared=True),
                                              confirm_allowed=False))
add("059-exec-conch-confirm-denied", execute("execute_shell_command",
                                             {"command": "ls", "timeout_ms": 5000},
                                             ctx([conch_device()]),
                                             confirm_allowed=False))

# =====================================================================================
# execute_shell_command — events
# =====================================================================================

add("060-events-conch-deltas", events("execute_shell_command",
                                      {"command": "ls", "timeout_ms": 5000},
                                      ctx([conch_device()]),
                                      conch_responses=[
                                          conch_response('{"job_id":"job-1"}'),
                                          conch_response(
                                              '{"state":"running","output":"bin\\n",'
                                              '"output_bytes":4}'),
                                          conch_response(
                                              '{"state":"succeeded","output":"bin\\netc\\n",'
                                              '"output_bytes":8}'),
                                      ]))
add("061-events-no-command", events("execute_shell_command", {"timeout_ms": 5000},
                                    ctx([conch_device()])))
add("062-events-no-timeout", events("execute_shell_command", {"command": "ls"},
                                    ctx([conch_device()])))
add("063-events-bad-timeout", events("execute_shell_command",
                                     {"command": "ls", "timeout_ms": "soon"},
                                     ctx([conch_device()])))
add("064-events-server-not-found", events("execute_shell_command",
                                          {"command": "ls", "server": "ghost",
                                           "timeout_ms": 5000},
                                          ctx([conch_device()])))
add("065-events-background", events("execute_shell_command",
                                    {"command": "sleep 10", "timeout_ms": 60000,
                                     "background": True},
                                    ctx([conch_device()]),
                                    conch_responses=[conch_response('{"job_id":"job-1"}')]))
add("066-events-background-ssh", events("execute_shell_command",
                                        {"command": "sleep 10", "timeout_ms": 60000,
                                         "background": True},
                                        ctx([ssh_device()])))
add("067-events-sandbox", events("execute_shell_command",
                                 {"command": "ls", "timeout_ms": 5000},
                                 ctx([], sandbox=True),
                                 sandbox={"execute_result": {"stdout": "bin\n",
                                                             "exit_code": 0}}))
add("068-events-sandbox-not-installed", events("execute_shell_command",
                                               {"command": "ls", "timeout_ms": 5000},
                                               ctx([], sandbox=True),
                                               sandbox={"available": False}))
add("069-events-sandbox-denied", events("execute_shell_command",
                                        {"command": "rm -rf build", "timeout_ms": 5000},
                                        ctx([], sandbox=True, shared=True),
                                        confirm_allowed=False))
add("070-events-durable", events("execute_shell_command",
                                 {"command": "echo hi", "timeout_ms": 5000},
                                 ctx([conch_device()]),
                                 conch_responses=[
                                     conch_response('{"job_id":"job-1"}'),
                                     conch_response(
                                         '{"state":"succeeded","output":"hi\\n","output_bytes":3}'),
                                 ]))
add("071-events-ssh", events("execute_shell_command",
                             {"command": "ls", "timeout_ms": 5000},
                             ctx([ssh_device()]),
                             jsch={"presented_key": "server-host-key",
                                   "stdout": "bin\n", "exit_status": 0}))

# =====================================================================================
# durable job tools
# =====================================================================================

add("080-list-jobs", execute("list_shell_jobs", {"server": "conch-box"},
                             ctx([conch_device()]),
                             conch_responses=[conch_response('{"jobs":[]}')]))
add("081-list-jobs-server-not-found", execute("list_shell_jobs", {"server": "ghost"},
                                              ctx([conch_device()])))
add("082-list-jobs-failure", execute("list_shell_jobs", {},
                                     ctx([conch_device()]),
                                     conch_responses=[conch_response("boom", code=500)]))
add("083-get-job", execute("get_shell_job", {"job_id": "job-1"},
                           ctx([conch_device()]),
                           conch_responses=[conch_response(
                               '{"job_id":"job-1","state":"succeeded","output":"done"}')]))
add("084-get-job-no-id", execute("get_shell_job", {}, ctx([conch_device()])))
add("085-get-job-failure", execute("get_shell_job", {"job_id": "job-1"},
                                   ctx([conch_device()]),
                                   conch_responses=[conch_response("boom", code=500)]))
add("086-wait-job-terminal", execute("wait_for_job",
                                     {"job_id": "job-1", "timeout_ms": 30000},
                                     ctx([conch_device()]),
                                     conch_responses=[conch_response(
                                         '{"state":"succeeded","output":"done",'
                                         '"output_bytes":4}')]))
add("087-wait-job-no-id", execute("wait_for_job", {"timeout_ms": 30000},
                                  ctx([conch_device()])))
add("088-wait-job-no-timeout", execute("wait_for_job", {"job_id": "job-1"},
                                       ctx([conch_device()])))
add("089-wait-job-bad-timeout", execute("wait_for_job",
                                        {"job_id": "job-1", "timeout_ms": "soon"},
                                        ctx([conch_device()])))
add("090-wait-job-clamped", execute("wait_for_job",
                                    {"job_id": "job-1", "timeout_ms": 600000},
                                    ctx([conch_device()], timeout=2000),
                                    conch_responses=[conch_response(
                                        '{"state":"succeeded","output":"done",'
                                        '"output_bytes":4}')]))
add("091-wait-job-poll-failure", execute("wait_for_job",
                                         {"job_id": "job-1", "timeout_ms": 30000},
                                         ctx([conch_device()]),
                                         conch_responses=[conch_response("boom", code=500)] * 5))
add("092-wait-job-events", events("wait_for_job",
                                  {"job_id": "job-1", "timeout_ms": 30000},
                                  ctx([conch_device()]),
                                  conch_responses=[conch_response(
                                      '{"state":"succeeded","output":"done",'
                                      '"output_bytes":4}')]))
add("093-stop-job", execute("stop_shell_job", {"job_id": "job-1"},
                            ctx([conch_device()]),
                            conch_responses=[conch_response('{"ok":true}')]))
add("094-stop-job-no-id", execute("stop_shell_job", {}, ctx([conch_device()])))
add("095-stop-job-denied", execute("stop_shell_job", {"job_id": "job-1"},
                                   ctx([conch_device()]),
                                   confirm_allowed=False))
add("096-stop-job-failure", execute("stop_shell_job", {"job_id": "job-1"},
                                    ctx([conch_device()]),
                                    conch_responses=[conch_response("boom", code=500)]))

# =====================================================================================
# file_read
# =====================================================================================

READ_BODY = ('{"content":"hello","lines":1,"total_lines":1,"total_bytes":5,'
             '"returned_bytes":5,"offset":0,"limit":1048576,"truncated":false}')
add("100-file-read-conch", execute("file_read", {"path": "/home/pi/a.txt"},
                                   ctx([conch_device()]),
                                   conch_responses=[conch_response(READ_BODY)]))
add("101-file-read-conch-error", execute("file_read", {"path": "/home/pi/a.txt"},
                                         ctx([conch_device()]),
                                         conch_responses=[conch_response(
                                             '{"error":"permission denied"}')]))
add("102-file-read-conch-throws", execute("file_read", {"path": "/home/pi/a.txt"},
                                          ctx([conch_device()]),
                                          conch_responses=[conch_response("boom", code=500)]))
add("103-file-read-ssh", execute("file_read", {"path": "/home/pi/a.txt"},
                                 ctx([ssh_device()]),
                                 jsch={"presented_key": "server-host-key",
                                       "pwd": "/home/pi",
                                       "stats": {"/home/pi/a.txt": {"size": 5}},
                                       "file_bytes": {"/home/pi/a.txt": "hello"}}))
add("104-file-read-ssh-missing", execute("file_read", {"path": "/home/pi/none.txt"},
                                         ctx([ssh_device()]),
                                         jsch={"presented_key": "server-host-key",
                                               "pwd": "/home/pi"}))
add("105-file-read-sandbox", execute("file_read", {"path": "/mnt/home/a.txt"},
                                     ctx([], sandbox=True),
                                     sandbox={"file_read_result": {
                                         "content": "sandbox", "lines": 1, "total_lines": 1,
                                         "total_bytes": 7, "returned_bytes": 7,
                                         "offset": 0, "limit": 1048576,
                                         "truncated": False}}))
add("106-file-read-sandbox-throws", execute("file_read", {"path": "/mnt/home/a.txt"},
                                            ctx([], sandbox=True),
                                            sandbox={"file_read_failure": "io error"}))
add("107-file-read-no-path", execute("file_read", {}, ctx([conch_device()])))
add("108-file-read-offset-limit", execute("file_read",
                                          {"path": "/home/pi/a.txt", "offset": 2, "limit": 10},
                                          ctx([conch_device()]),
                                          conch_responses=[conch_response(
                                              '{"content":"llo","lines":1,"total_lines":1,'
                                              '"total_bytes":5,"returned_bytes":3,'
                                              '"offset":2,"limit":10,"truncated":true}')]))

# =====================================================================================
# file_write
# =====================================================================================

add("110-file-write-conch", execute("file_write",
                                    {"path": "/home/pi/new.txt", "content": "data"},
                                    ctx([conch_device()]),
                                    conch_responses=[conch_response("{}")]))
add("111-file-write-conch-error", execute("file_write",
                                          {"path": "/home/pi/new.txt", "content": "data"},
                                          ctx([conch_device()]),
                                          conch_responses=[conch_response(
                                              '{"error":"disk full"}')]))
add("112-file-write-ssh", execute("file_write",
                                  {"path": "/home/pi/new.txt", "content": "data"},
                                  ctx([ssh_device()]),
                                  jsch={"presented_key": "server-host-key",
                                        "stats": {"/home/pi": {"is_dir": True}}}))
add("113-file-write-sandbox", execute("file_write",
                                      {"path": "/mnt/home/new.txt", "content": "data"},
                                      ctx([], sandbox=True),
                                      sandbox={"file_write_error": None}))
add("114-file-write-sandbox-error", execute("file_write",
                                            {"path": "/mnt/home/new.txt", "content": "data"},
                                            ctx([], sandbox=True),
                                            sandbox={"file_write_error": "no space"}))
add("115-file-write-denied", execute("file_write",
                                     {"path": "/mnt/shared/secret.txt", "content": "data"},
                                     ctx([], sandbox=True, shared=True),
                                     confirm_allowed=False))
add("116-file-write-shared-allowed", execute("file_write",
                                             {"path": "/mnt/shared/secret.txt",
                                              "content": "data"},
                                             ctx([], sandbox=True, shared=True),
                                             sandbox={"file_write_error": None}))
add("117-file-write-no-content", execute("file_write", {"path": "/a.txt"},
                                         ctx([conch_device()])))
add("118-file-write-no-path", execute("file_write", {"content": "x"}, ctx([conch_device()])))
add("119-file-write-too-large", execute("file_write",
                                        {"path": "/a.txt", "content": "x" * 1048577},
                                        ctx([conch_device()])))
add("120-file-write-conch-denied", execute("file_write",
                                           {"path": "/home/pi/new.txt", "content": "data"},
                                           ctx([conch_device()]),
                                           confirm_allowed=False))

# =====================================================================================
# file_edit
# =====================================================================================

add("130-file-edit-conch", execute("file_edit",
                                   {"path": "/home/pi/a.txt", "old_string": "foo",
                                    "new_string": "bar"},
                                   ctx([conch_device()]),
                                   conch_responses=[conch_response(
                                       '{"replacements":1,"sha256":"abc"}')]))
add("131-file-edit-conch-error", execute("file_edit",
                                         {"path": "/home/pi/a.txt", "old_string": "foo",
                                          "new_string": "bar"},
                                         ctx([conch_device()]),
                                         conch_responses=[conch_response(
                                             '{"replacements":0,"sha256":"",'
                                             '"error":"old_string not found"}')]))
add("132-file-edit-ssh", execute("file_edit",
                                 {"path": "/home/pi/a.txt", "old_string": "foo",
                                  "new_string": "bar"},
                                 ctx([ssh_device()]),
                                 jsch={"presented_key": "server-host-key",
                                       "stats": {"/home/pi/a.txt": {"size": 7}},
                                       "file_bytes": {"/home/pi/a.txt": "foo bar"},
                                       "file_bytes_second": {"/home/pi/a.txt": "foo bar"}}))
add("133-file-edit-sandbox", execute("file_edit",
                                     {"path": "/mnt/home/a.txt", "old_string": "foo",
                                      "new_string": "bar"},
                                     ctx([], sandbox=True),
                                     sandbox={"file_edit_result": {"replacements": 1,
                                                                   "sha256": "abc"}}))
add("134-file-edit-no-old", execute("file_edit", {"path": "/a.txt", "new_string": "x"},
                                    ctx([conch_device()])))
add("135-file-edit-no-path", execute("file_edit", {"old_string": "x", "new_string": "y"},
                                     ctx([conch_device()])))
add("136-file-edit-replace-all", execute("file_edit",
                                         {"path": "/home/pi/a.txt", "old_string": "foo",
                                          "new_string": "bar", "replace_all": "TRUE"},
                                         ctx([conch_device()]),
                                         conch_responses=[conch_response(
                                             '{"replacements":3,"sha256":"def"}')]))
add("137-file-edit-denied", execute("file_edit",
                                    {"path": "/mnt/shared/a.txt", "old_string": "foo",
                                     "new_string": "bar"},
                                    ctx([], sandbox=True, shared=True),
                                    confirm_allowed=False))

# =====================================================================================
# file_glob
# =====================================================================================

add("140-file-glob-conch", execute("file_glob", {"pattern": "*.txt"},
                                   ctx([conch_device()]),
                                   conch_responses=[conch_response(
                                       '{"ok":true,"files":["/home/pi/a.txt",'
                                       '"/home/pi/b.txt"],"truncated":false}')]))
add("141-file-glob-conch-error", execute("file_glob", {"pattern": "*.txt"},
                                         ctx([conch_device()]),
                                         conch_responses=[conch_response(
                                             '{"error":"bad glob"}')]))
add("142-file-glob-ssh", execute("file_glob", {"pattern": "*.txt"},
                                 ctx([ssh_device()]),
                                 jsch={"presented_key": "server-host-key",
                                       "pwd": "/home/pi",
                                       "ls": {"/home/pi": [["a.txt", "false"],
                                                           ["b.txt", "false"]]}}))
add("143-file-glob-sandbox", execute("file_glob", {"pattern": "*.txt", "depth": 2},
                                     ctx([], sandbox=True),
                                     sandbox={"file_glob_result": {
                                         "files": ["/mnt/home/a.txt"],
                                         "truncated": True}}))
add("144-file-glob-no-pattern", execute("file_glob", {}, ctx([conch_device()])))
add("145-file-glob-bad-depth", execute("file_glob", {"pattern": "*.txt", "depth": "x"},
                                       ctx([conch_device()]),
                                       conch_responses=[conch_response(
                                           '{"ok":true,"files":[],"truncated":false}')]))

# =====================================================================================
# file_grep
# =====================================================================================

add("150-file-grep-conch", execute("file_grep", {"pattern": "needle"},
                                   ctx([conch_device()]),
                                   conch_responses=[conch_response(
                                       '{"ok":true,"matches":[{"path":"/home/pi/a.txt",'
                                       '"line":1,"content":"found needle"}],'
                                       '"truncated":false}')]))
add("151-file-grep-conch-error", execute("file_grep", {"pattern": "needle"},
                                         ctx([conch_device()]),
                                         conch_responses=[conch_response(
                                             '{"error":"bad regex"}')]))
add("152-file-grep-ssh-server", execute("file_grep", {"pattern": "needle"},
                                        ctx([ssh_device()]),
                                        jsch={"presented_key": "server-host-key",
                                              "pwd": "/home/pi",
                                              "exit_status": 0,
                                              "stdout": "/home/pi/a.txt:1:found needle\n"}))
add("153-file-grep-ssh-fallback", execute("file_grep", {"pattern": "needle"},
                                          ctx([ssh_device()]),
                                          jsch={"presented_key": "server-host-key",
                                                "pwd": "/home/pi",
                                                "exit_status": 2,
                                                "ls": {".": [["a.txt", "false"]]},
                                                "stats": {"./a.txt": {"size": 12}},
                                                "file_bytes": {"./a.txt": "found needle"}}))
add("154-file-grep-sandbox", execute("file_grep", {"pattern": "needle"},
                                     ctx([], sandbox=True),
                                     sandbox={"file_grep_result": {
                                         "matches": [{"path": "/mnt/home/a.txt",
                                                      "line": 1,
                                                      "content": "found needle"}],
                                         "truncated": False}}))
add("155-file-grep-sandbox-error", execute("file_grep", {"pattern": "needle"},
                                           ctx([], sandbox=True),
                                           sandbox={"file_grep_failure": "grep failed"}))
add("156-file-grep-no-pattern", execute("file_grep", {}, ctx([conch_device()])))
add("157-file-grep-options", execute("file_grep",
                                     {"pattern": "needle", "path": "/srv",
                                      "glob": "*.kt"},
                                     ctx([conch_device()]),
                                     conch_responses=[conch_response(
                                         '{"ok":true,"matches":[],"truncated":true}')]))

# =====================================================================================
# view_image
# =====================================================================================

add("160-view-image", events("view_image", {"path": "/home/pi/img.png"},
                             ctx([conch_device()]),
                             conch_responses=[conch_response(
                                 '{"data":"aGVsbG8=","mimeType":"image/png","size":5}')],
                             image={"result": {"path": "/files/tool-media/conch.png",
                                               "mime_type": "image/png", "size": 5,
                                               "width": 64, "height": 32,
                                               "sha256": "abc"}}))
add("161-view-image-no-store", events("view_image", {"path": "/home/pi/img.png"},
                                      ctx([conch_device()]),
                                      conch_responses=[conch_response(
                                          '{"data":"aGVsbG8=","mimeType":"image/png",'
                                          '"size":5}')],
                                      no_image_store=True))
add("162-view-image-remote-error", events("view_image", {"path": "/home/pi/img.png"},
                                          ctx([conch_device()]),
                                          conch_responses=[conch_response(
                                              '{"error":"not an image"}')]))
add("163-view-image-persist-failure", events("view_image", {"path": "/home/pi/img.png"},
                                             ctx([conch_device()]),
                                             conch_responses=[conch_response(
                                                 '{"data":"aGVsbG8=",'
                                                 '"mimeType":"image/png","size":5}')],
                                             image={"failure": "decode failed"}))
add("164-view-image-no-path", events("view_image", {}, ctx([conch_device()])))
add("165-view-image-server-not-found", events("view_image", {"path": "/a.png",
                                                             "server": "ghost"},
                                              ctx([conch_device()])))
add("166-view-image-execute-text", execute("view_image", {"path": "/home/pi/img.png"},
                                           ctx([conch_device()]),
                                           conch_responses=[conch_response(
                                               '{"data":"aGVsbG8=","mimeType":"image/png",'
                                               '"size":5}')]))

# =====================================================================================
# acknowledgeCommittedJobs
# =====================================================================================

TERMINAL_RESULT = ('{"type":"execute_shell_command","server":"conch-box",'
                   '"command":"ls","job_id":"job-1","durable":true,'
                   '"result":{"state":"succeeded"}}')
RUNNING_RESULT = ('{"type":"execute_shell_command","server":"conch-box",'
                  '"command":"ls","job_id":"job-2","durable":true,'
                  '"result":{"state":"running"}}')


def acknowledge(calls, context, **extra):
    scenario = {"op": "acknowledge", "context": context, "calls": calls}
    scenario.update(extra)
    return scenario


add("170-ack-terminal", acknowledge(
    [{"toolName": "execute_shell_command", "arguments": '{"server":"conch-box"}',
      "result": TERMINAL_RESULT}],
    ctx([conch_device()]),
    conch_responses=[conch_response('{"ok":true}')]))
add("171-ack-running", acknowledge(
    [{"toolName": "execute_shell_command", "arguments": '{"server":"conch-box"}',
      "result": RUNNING_RESULT}],
    ctx([conch_device()])))
add("172-ack-failure-swallowed", acknowledge(
    [{"toolName": "execute_shell_command", "arguments": '{"server":"conch-box"}',
      "result": TERMINAL_RESULT}],
    ctx([conch_device()]),
    conch_responses=[conch_response("boom", code=500)]))
add("173-ack-server-not-found", acknowledge(
    [{"toolName": "execute_shell_command", "arguments": '{"server":"ghost"}',
      "result": TERMINAL_RESULT}],
    ctx([conch_device()])))
add("174-ack-deduplicated", acknowledge(
    [{"toolName": "execute_shell_command", "arguments": '{"server":"conch-box"}',
      "result": TERMINAL_RESULT},
     {"toolName": "execute_shell_command", "arguments": '{"server":"CONCH-BOX"}',
      "result": TERMINAL_RESULT}],
    ctx([conch_device()]),
    conch_responses=[conch_response('{"ok":true}')]))
add("175-ack-not-shell-tool", acknowledge(
    [{"toolName": "file_read", "arguments": '{"server":"conch-box"}',
      "result": TERMINAL_RESULT}],
    ctx([conch_device()])))

for name, scenario in scenarios:
    path = os.path.join(OUT, f"{name}.json")
    with open(path, "w") as f:
        json.dump(scenario, f, ensure_ascii=False, separators=(",", ":"))

print(f"Generated {len(scenarios)} scenarios in {OUT}")
