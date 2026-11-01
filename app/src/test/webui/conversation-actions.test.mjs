import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

function fixture() {
  const sent = [];
  const context = vm.createContext({
    location: { protocol: "http:", host: "localhost" },
    sessionSignedIn: async () => true,
    performance: { now: () => 0 }, t: {},
    setTimeout: () => 0, clearTimeout() {},
    WebSocket: class { static OPEN = 1; },
  });
  const source = fs.readFileSync(new URL("../../main/assets/webui/sync.js", import.meta.url), "utf8");
  vm.runInContext(source.replace(/^import .*;\r?\n/gm, "").replace(/export /g, ""), context);
  context.transport = { readyState: 1, send: text => sent.push(JSON.parse(text)) };
  vm.runInContext('socket = transport; update({ connected: true, openId: "running", openStatus: "ready", conversations: [{ id: "running", generating: true }], generating: true })', context);
  return { sent, evaluate: code => vm.runInContext(code, context) };
}

test("running conversations route pin, title, rename and delete without a generation guard", () => {
  const f = fixture();
  f.evaluate('sync.pin("running", true); sync.generateTitle("running"); sync.renameConversation("running", "Named"); sync.deleteConversation("background")');
  assert.deepEqual(f.sent.map(command => command.type), ["pin", "conversation_title", "conversation_rename", "conversation_delete"]);
  assert.equal(f.sent[1].conversationId, "running");
});

test("title routing still refuses loading and disconnected requests", () => {
  const f = fixture();
  assert.equal(f.evaluate('update({ openStatus: "loading" }); sync.generateTitle("running")'), false);
  assert.equal(f.evaluate('update({ openStatus: "ready", connected: false }); sync.generateTitle("running")'), false);
  assert.equal(f.sent.length, 0);
});

test("running selected delete keeps confirmation covering and rejects duplicate commands", () => {
  const f = fixture();
  f.evaluate('sync.deleteConversation("running")');
  assert.equal(f.sent.length, 0);
  assert.equal(f.evaluate('state.switching.phase'), "covering");
  assert.equal(f.evaluate('state.switching.confirmation'), true);
  assert.equal(f.evaluate('sync.deleteConversation("running")'), null);
  f.evaluate('sync.covered(state.switching.id)');
  assert.equal(f.sent.length, 1);
  assert.equal(f.sent[0].type, "conversation_delete");
});

function childFixture() {
  const f = fixture();
  f.evaluate(`openSeq = 7; update({ connectionId: "session", openId: "child", openStatus: "ready",
    parentConversationId: "parent", subagents: [{ id: "child", title: "Child" }],
    composer: { conversationId: "child", seq: 7, phase: "IDLE", attachments: [], queue: [] } });
    sync.deleteConversation("child"); sync.covered(state.switching.id);`);
  const command = f.sent[0];
  assert.equal(command.type, "conversation_delete");
  assert.equal(command.conversationId, "child");
  assert.equal(f.evaluate("state.switching.phase"), "loading");
  return { ...f, command,
    receive(event) { f.evaluate(`receive(${JSON.stringify(event)}, 0)`); },
  };
}
const parentPath = { type: "path", conversationId: "parent", seq: 7, generating: false,
  messages: [{ id: "parent-message", parentId: null }] };
const parentComposer = { type: "composer", conversationId: "parent", seq: 7,
  phase: "IDLE", actionId: 0, editRevision: 0, text: "", generating: false };

test("child conversation deletion retains confirmation across opened parent until exact receipt and exit", () => {
  for (const receiptFirst of [false, true]) {
    const f = childFixture(), id = f.command.actionId;
    const success = { type: "conversation_delete", conversationId: "child", actionId: id, success: true };
    if (receiptFirst) f.receive(success);
    assert.equal(f.evaluate("state.switching.confirmation"), true);
    assert.equal(f.evaluate("state.switching.phase"), "loading");
    f.receive({ type: "opened", conversationId: "parent", seq: 7 });
    assert.equal(f.evaluate("state.openId"), "parent");
    assert.equal(f.evaluate("state.openStatus"), "loading");
    assert.equal(f.evaluate("state.switching.id"), id);
    assert.equal(f.evaluate("state.switching.command.conversationId"), "child");
    assert.equal(f.evaluate("state.switching.confirmation"), true);
    assert.equal(f.evaluate("state.switching.phase"), "loading");
    if (!receiptFirst) {
      f.receive({ ...success, conversationId: "parent" });
      f.receive({ ...success, actionId: id + 99 });
      assert.equal(f.evaluate("state.conversationDelete.actionId"), id);
      assert.equal(f.evaluate("state.conversationDeleteResult"), null);
    }
    f.receive(parentPath);
    f.receive(parentComposer);
    assert.equal(f.evaluate("state.openId"), "parent");
    assert.equal(f.evaluate("state.openStatus"), "ready");
    assert.equal(f.evaluate("state.path[0].id"), "parent-message");
    assert.equal(f.evaluate("state.composer.conversationId"), "parent");
    assert.equal(f.evaluate("state.switching.confirmation"), true);
    if (!receiptFirst) {
      assert.equal(f.evaluate("state.switching.phase"), "loading");
      f.receive(success);
    }
    assert.equal(f.evaluate("state.switching.phase"), "loading");
    f.receive({ type: "todo", conversationId: "parent", seq: 7, items: [] });
    f.receive({ type: "subagents", conversationId: "parent", seq: 7, parentConversationId: null, items: [] });
    assert.equal(f.evaluate("state.switching.phase"), "loading");
    f.receive({ type: "interactions", conversationId: "parent", seq: 7, items: [], actionId: 0 });
    assert.equal(f.evaluate("state.switching.phase"), "settling");
    assert.equal(f.evaluate("state.conversationDelete"), null);
    assert.equal(f.evaluate("state.conversationDeleteResult.actionId"), id);
    assert.equal(f.evaluate("state.switching.confirmation"), true);
    f.evaluate(`sync.confirmationExited(${id + 99})`);
    assert.equal(f.evaluate("state.switching.confirmation"), true);
    f.evaluate(`sync.confirmationExited(${id})`);
    assert.equal(f.evaluate("state.switching.confirmation"), false);
    assert.equal(f.evaluate("state.switching.phase"), "settling");
    f.evaluate(`sync.settled(${id}); sync.revealed(${id})`);
    assert.equal(f.evaluate("state.switching"), null);
    assert.equal(f.evaluate("state.openId"), "parent");
    assert.deepEqual(f.sent.map(command => command.type), ["conversation_delete"]);
  }
});

test("child delete parent-return cannot overwrite a newer explicit conversation or New Chat sequence", () => {
  for (const destination of ["newer", null]) {
    const f = childFixture(), deleteId = f.command.actionId;
    f.evaluate(`sync.open(${JSON.stringify(destination)})`);
    if (destination !== null) f.evaluate("sync.covered(state.switching.id)");
    assert.equal(f.evaluate("openSeq"), 8);
    assert.equal(f.evaluate("state.openId"), destination);
    const switchId = f.evaluate("state.switching?.id ?? null");
    f.receive({ type: "opened", conversationId: "parent", seq: 7 });
    f.receive(parentPath); f.receive(parentComposer);
    f.receive({ type: "conversation_delete", conversationId: "child", actionId: deleteId, success: true });
    f.evaluate(`sync.confirmationExited(${deleteId}); sync.settled(${deleteId}); sync.revealed(${deleteId})`);
    assert.equal(f.evaluate("state.openId"), destination);
    assert.equal(f.evaluate("openSeq"), 8);
    assert.equal(f.evaluate("state.switching?.id ?? null"), switchId);
    f.receive({ ...parentComposer, conversationId: destination, seq: 8 });
    if (destination !== null) f.receive({ ...parentPath, conversationId: destination, seq: 8,
      messages: [{ id: "newer-message", parentId: null }] });
    f.receive({ type: "opened", conversationId: "parent", seq: 7 });
    assert.equal(f.evaluate("state.openId"), destination);
    assert.equal(f.evaluate("state.composer.conversationId"), destination);
    if (destination !== null) assert.equal(f.evaluate("state.path[0].id"), "newer-message");
    assert.deepEqual(f.sent.map(command => command.type), ["conversation_delete", "open"]);
  }
});
