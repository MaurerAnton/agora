#!/usr/bin/env python3
"""Generates the conversation-runtime scenarios. Deterministic; rerun after editing.

Every scenario starts from `RunState.Idle` and replays a command sequence. The flows below walk
the reducer's states deliberately: recovery, slot acquisition and Run binding, Send placement and
guidance, provider passes, the tool-batch/commit gate, natural finalization, Stop and its two
settlement barriers, and the rejection paths (illegal state, stale identity, duplicate result).

Command identities are explicit because the reducer compares them field by field. Two derived ids
matter:

* recovery emits `recover-<runId>-<pass>`;
* a tool batch derives `tool-batch-<effectId>` from the request, and the commit derives
  `tool-round-<batchEffectId>` from the accepted batch.

A command that violates a Kotlin `require` in the command type aborts the driver, so the scenarios
are also a check that the public command surface accepts them.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

LONG_MAX = 9223372036854775807
CID = "c"

ALL = []


def case(name, commands, conversation_id=CID):
    ALL.append({"name": name, "conversationId": conversation_id, "commands": commands})


def cmd(kind, identity=None, **kw):
    entry = {"kind": kind}
    if identity is not None:
        entry["identity"] = identity
    entry.update(kw)
    return entry


def eid(run="run1", owner=1, pass_=0, effect="e", cid=CID):
    return {"conversationId": cid, "ownerToken": owner, "runId": run, "pass": pass_,
            "effectId": effect}


def rid(run=None, owner=1, pass_=0, cid=CID):
    identity = {"conversationId": cid, "ownerToken": owner, "pass": pass_}
    if run is not None:
        identity["runId"] = run
    return identity


def acquire(owner=1):
    return cmd("AcquireSlot", rid(run=None, owner=owner))


def bind(run="run1", owner=1, pass_=0):
    return cmd("BindRun", rid(run=run, owner=owner, pass_=pass_))


def send(effect="send", run="prop", owner=1, direct=False, guidance=False):
    return cmd("SendRequested", eid(run=run, owner=owner, pass_=0, effect=effect),
               directOnly=direct, hasPendingGuidance=guidance)


def active_bound(run="run1", owner=1):
    return [acquire(owner=owner), bind(run=run, owner=owner)]


def stop(effect="stop", run="run1", owner=1, coroutine_settled=False,
         requires_persistence=True):
    return cmd(
        "StopRequested",
        rid(run=run, owner=owner),
        coroutineAlreadySettled=coroutine_settled,
        requiresPersistence=requires_persistence,
        effectId=effect if requires_persistence else None,
    )


def recover(run="run1", pass_=0, status="ACTIVE"):
    return cmd("Recover", snapshot={
        "conversationId": CID, "runId": run, "pass": pass_, "status": status,
    })


def recovered(run="run1", pass_=0, success=True, owner=LONG_MAX, effect=None):
    return cmd(
        "RecoveryCompleted",
        eid(run=run, owner=owner, pass_=pass_,
            effect=effect if effect is not None else f"recover-{run}-{pass_}"),
        success=success,
    )


def provider_request(effect="pp", run="run1", pass_=0):
    return cmd("ProviderPassRequested", eid(run=run, pass_=pass_, effect=effect))


def provider_done(effect="pp", result="COMPLETED_TEXT", run="run1", pass_=0):
    return cmd("ProviderPassCompleted", eid(run=run, pass_=pass_, effect=effect),
               result=result)


def tool_request(effect="tb", run="run1", pass_=0):
    return cmd("ToolBatchRequested", eid(run=run, pass_=pass_, effect=effect))


def tool_done(effect="tb", run="run1", pass_=0):
    # The completed command carries the *effect* identity the reducer derived for the batch.
    return cmd("ToolBatchCompleted",
               eid(run=run, pass_=pass_, effect=f"tool-batch-{effect}"))


def round_done(effect="tb", success=True, run="run1", pass_=0):
    return cmd("ToolRoundCommitted",
               eid(run=run, pass_=pass_, effect=f"tool-round-tool-batch-{effect}"),
               success=success)


def finalize_request(effect="fin", status="COMPLETED", reason="MODEL_COMPLETED",
                     unread=True, run="run1", pass_=0):
    return cmd("FinalizationRequested", eid(run=run, pass_=pass_, effect=effect),
               status=status, reason=reason, markConversationUnread=unread)


def finalize_done(effect="fin", success=True, run="run1", pass_=0):
    return cmd("FinalizationCompleted", eid(run=run, pass_=pass_, effect=effect),
               success=success)


def coroutine_settled(run="run1", owner=1, pass_=0):
    return cmd("CoroutineSettled", rid(run=run, owner=owner, pass_=pass_))


def persistence_settled(effect="stop", success=True, run="run1", pass_=0):
    return cmd("PersistenceSettled", eid(run=run, pass_=pass_, effect=effect),
               success=success)


# ---- recovery -------------------------------------------------------------------------------
case("recovery_success", [recover(), recovered(), acquire()])
case("recovery_failure_then_duplicate", [recover(), recovered(success=False),
                                         recovered(success=False)])
case("recovery_stale_identity", [recover(), recovered(effect="wrong")])
case("recovery_duplicate_snapshot", [recover(), recover(),
                                     recover(pass_=1)])
case("recovery_rejects_other_commands", [recover(), acquire(), send(),
                                         coroutine_settled(run=None)])

# ---- slot acquisition and binding -----------------------------------------------------------
case("acquire_slot_rejected_when_active", [acquire(), acquire()])
case("bind_run_then_duplicates", [acquire(), bind(), bind(), bind(owner=2)])
case("bind_run_rejects_another_run", [acquire(), bind(), bind(run="run2")])
case("bind_run_allows_pass_advance", [acquire(), bind(pass_=0), bind(pass_=1),
                                      bind(pass_=3)])
case("bind_run_rejected_when_coroutine_settled",
     [acquire(), bind(), coroutine_settled(), bind(pass_=1)])
case("bind_run_rejected_during_provider", [acquire(), bind(), provider_request(),
                                           bind(pass_=1)])

# ---- send placement and guidance ------------------------------------------------------------
case("send_preparing_flow", [send(), cmd("InputPersisted", eid(run="prop", effect="send")),
                             acquire()])
case("send_busy_direct_with_guidance", [send(direct=True, guidance=True)])
case("send_drains_guidance_first", [send(guidance=True)])
case("send_guidance_while_preparing",
     [send(), send(direct=True, effect="g1"), send(direct=False, effect="g2")])
case("send_guidance_while_unbound_active",
     [acquire(), send(direct=False), send(direct=True)])
case("send_guidance_while_bound", [acquire(), bind(), send(direct=False, effect="g")])
case("send_rejected_while_recovering", [recover(), send()])
case("send_launch_abandoned", [send(),
                               cmd("SendLaunchAbandoned", eid(run="prop", effect="send")),
                               cmd("SendLaunchAbandoned", eid(run="prop", effect="send"))])
case("input_persistence_failed", [send(),
                                  cmd("InputPersistenceFailed", eid(run="prop", effect="send")),
                                  cmd("InputPersistenceFailed", eid(run="prop", effect="send")),
                                  cmd("InputPersistenceFailed", eid(run="prop", effect="other"))])
case("input_persisted_rejected_outside_preparing",
     [acquire(), cmd("InputPersisted", eid(run="prop", effect="send"))])
case("input_persisted_stale_identity", [send(),
                                        cmd("InputPersisted", eid(run="prop", effect="other"))])

# ---- provider passes ------------------------------------------------------------------------
case("provider_pass_happy", [acquire(), bind(), provider_request(),
                             provider_done(), provider_request(effect="pp2")])
case("provider_pass_duplicate_and_stale",
     [acquire(), bind(), provider_request(),
      provider_request(), provider_request(effect="pp2"), provider_done(effect="pp2")])
case("provider_pass_requires_running_phase", [acquire(), bind(), provider_done()])
case("provider_pass_rejected_when_tool_phase",
     [acquire(), bind(), tool_request(), provider_request()])

# ---- tool batch and commit gate -------------------------------------------------------------
case("tool_batch_happy",
     [acquire(), bind(), provider_request(), provider_done(),
      tool_request(), tool_done(), round_done(), provider_request(effect="pp2")])
case("tool_batch_duplicates",
     [acquire(), bind(), tool_request(), tool_request(), tool_request(effect="tb2")])
case("tool_batch_completed_stale",
     [acquire(), bind(), tool_request(), tool_done(effect="wrong"), tool_done(),
      tool_done()])
case("tool_round_failure_then_retry",
     [acquire(), bind(), tool_request(), tool_done(),
      round_done(success=False), round_done(success=False), round_done(success=True)])
case("tool_round_rejected_in_executing_phase",
     [acquire(), bind(), tool_request(), round_done()])
case("tool_batch_rejected_when_provider_running",
     [acquire(), bind(), provider_request(), tool_request()])

# ---- natural finalization -------------------------------------------------------------------
case("finalization_happy", [acquire(), bind(), finalize_request(), finalize_done(),
                            coroutine_settled()])
case("finalization_coroutine_first", [acquire(), bind(), finalize_request(),
                                      coroutine_settled(), finalize_done()])
case("finalization_failure_then_duplicate",
     [acquire(), bind(), finalize_request(), finalize_done(success=False),
      finalize_done(success=False)])
case("finalization_stale_identity",
     [acquire(), bind(), finalize_request(), finalize_done(effect="wrong"),
      finalize_done(success=False), finalize_done(effect="wrong")])
case("finalization_rejected_outside_active",
     [acquire(), bind(), finalize_request(), finalize_request()])
case("finalization_terminal_variants",
     [acquire(), bind(),
      finalize_request(effect="f1", status="STOPPED", reason="USER_STOPPED", unread=False),
      finalize_done(effect="f1"), coroutine_settled(),
      acquire(owner=2), bind(run="run2", owner=2),
      finalize_request(effect="f2", status="FAILED", reason="PROVIDER_ERROR", run="run2"),
      coroutine_settled(run="run2", owner=2), finalize_done(effect="f2", run="run2")])

# ---- stop: bound Run ------------------------------------------------------------------------
case("stop_bound_persistence_then_coroutine",
     [acquire(), bind(), stop(), persistence_settled(), coroutine_settled()])
case("stop_bound_coroutine_then_persistence",
     [acquire(), bind(), stop(), coroutine_settled(), persistence_settled()])
case("stop_persistence_failure_then_success",
     [acquire(), bind(), stop(), persistence_settled(success=False),
      persistence_settled(success=False), persistence_settled(),
      coroutine_settled()])
case("stop_duplicate_and_stale",
     [acquire(), bind(), stop(), stop(), stop(effect="other"),
      stop(run="run2")])
case("stop_retry_after_failure",
     [acquire(), bind(), stop(), persistence_settled(success=False), stop(), stop()])

# ---- stop: unbound slot ---------------------------------------------------------------------
case("stop_empty_unbound",
     [acquire(), stop(run=None, requires_persistence=False, coroutine_settled=True)])
case("stop_unbound_then_bind_run",
     [acquire(), stop(run=None, requires_persistence=False),
      bind(), persistence_settled(effect="stop-1"), coroutine_settled()])
case("stop_unbound_binds_with_another_owner",
     [acquire(), stop(run=None, requires_persistence=False), bind(owner=2)])
case("stop_from_preparing_then_input_persisted",
     [send(), stop(run=None, requires_persistence=False),
      cmd("InputPersisted", eid(run="prop", effect="send")),
      persistence_settled(effect="stop-1"), coroutine_settled(run="prop"),
      persistence_settled(effect="stop-1", run="prop")])
case("stop_from_recovering_is_illegal", [recover(), stop(run=None, requires_persistence=False)])
case("stop_while_finalizing_requires_failure",
     [acquire(), bind(), finalize_request(), stop(run="run1", effect="stop2")])
case("stop_while_finalizing_after_failure",
     [acquire(), bind(), finalize_request(), finalize_done(success=False),
      stop(run="run1", effect="stop2"), persistence_settled(effect="stop2"),
      coroutine_settled()])
case("stop_from_idle_is_illegal", [stop(run=None, requires_persistence=False)])

# ---- coroutine settlement -------------------------------------------------------------------
case("settle_coroutine_while_preparing",
     [send(), coroutine_settled(run=None)])
case("settle_coroutine_while_preparing_stale",
     [send(), coroutine_settled(run=None, owner=2)])
case("settle_coroutine_unbound_active", [acquire(), coroutine_settled(run=None)])
case("settle_coroutine_bound_active",
     [acquire(), bind(), coroutine_settled(), coroutine_settled(),
      coroutine_settled(owner=2)])
case("settle_coroutine_while_idle", [coroutine_settled(run=None)])

# ---- persistence settlement outside a Stop --------------------------------------------------
case("persistence_settled_outside_stopping", [acquire(), persistence_settled()])
case("persistence_settled_without_effect_id",
     [acquire(), stop(run=None, requires_persistence=False), persistence_settled()])

# ---- crossings ------------------------------------------------------------------------------
case("send_while_stopping", [acquire(), bind(), stop(), send(direct=False),
                             send(direct=True)])
case("send_while_finalizing", [acquire(), bind(), finalize_request(), send(direct=False),
                               send(direct=True)])
case("bind_run_rejects_another_owner", [acquire(owner=1), bind(owner=2)])
case("stale_conversation_is_rejected_first",
     [cmd("AcquireSlot", rid(run=None, cid="other"))])
case("stop_while_too_early_but_bound_is_stale",
     [acquire(), stop(run="run1", effect="stop")])

os.makedirs(SCENARIOS_DIR, exist_ok=True)
for stale in os.listdir(SCENARIOS_DIR):
    if stale.endswith(".json"):
        os.remove(os.path.join(SCENARIOS_DIR, stale))
for entry in ALL:
    path = os.path.join(SCENARIOS_DIR, entry["name"] + ".json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(entry, handle, ensure_ascii=False)
        handle.write("\n")
print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")
