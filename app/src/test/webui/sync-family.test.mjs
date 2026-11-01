import assert from "node:assert/strict";
import fs from "node:fs";
import test from "node:test";
import vm from "node:vm";

function fixture() {
  const sockets = [], sent = [], timers = new Map();
  let timer = 0;
  const context = vm.createContext({
    location: { protocol: "http:", host: "localhost" },
    sessionSignedIn: async () => true,
    performance: { now: () => 0 }, t: {},
    setTimeout(callback) { timers.set(++timer, callback); return timer; },
    clearTimeout(id) { timers.delete(id); },
    WebSocket: class {
      static OPEN = 1;
      readyState = 1;
      constructor() { sockets.push(this); }
      send(message) { sent.push(JSON.parse(message)); }
      close() {}
    },
  });
  const source = fs.readFileSync(new URL("../../main/assets/webui/sync.js", import.meta.url), "utf8");
  vm.runInContext(source.replace(/^import .*;\r?\n/gm, "").replace(/export /g, ""), context);
  const evaluate = code => vm.runInContext(code, context);
  const current = () => evaluate("state");
  const receive = event => sockets[sockets.length - 1].onmessage({ data: JSON.stringify(event) });
  evaluate("sync.start(() => {})");
  sockets[0].onopen();
  receive({ type: "connection", connectionId: "session" });
  function navigate(id) {
    context.targetId = id;
    evaluate("sync.open(targetId)");
    const request = current().switching;
    if (request?.phase === "covering") evaluate(`sync.covered(${request.id})`);
    return current();
  }
  const open = navigate("root");
  receive({ type: "subagents", conversationId: "root", seq: open.switching.command.seq,
    parentConversationId: null, items: [{ id: "a", title: "Alpha", status: "idle" }, { id: "b", title: "Beta", status: "running" }] });
  return { current, receive, navigate, evaluate, sent,
    async disconnect() { await sockets[sockets.length - 1].onclose({ code: 1006 }); },
    reconnect() {
      const callback = [...timers.values()][0];
      timers.clear(); callback(); sockets[sockets.length - 1].onopen();
    },
  };
}

test("sync retains the proven family while changing selected parent identity before acknowledgements", () => {
  const f = fixture(), agents = f.current().subagents;
  let selected = f.navigate("a");
  assert.equal(selected.openId, "a");
  assert.equal(selected.parentConversationId, "root");
  assert.equal(selected.subagents, agents);
  selected = f.navigate("b");
  assert.equal(selected.parentConversationId, "root");
  assert.equal(selected.subagents, agents);
  selected = f.navigate("root");
  assert.equal(selected.parentConversationId, null);
  assert.equal(selected.subagents, agents);
  assert.deepEqual(f.sent.filter(command => command.type === "open").map(command => command.conversationId),
    [null, "root", "a", "b", "root"]);
});

test("sync retains sequence fences through rapid family navigation and accepts only current rows", () => {
  const f = fixture(), original = f.current().subagents;
  const childSeq = f.navigate("a").switching.command.seq;
  const siblingSeq = f.navigate("b").switching.command.seq;
  f.receive({ type: "subagents", conversationId: "a", seq: childSeq, parentConversationId: "wrong", items: [] });
  f.receive({ type: "subagents", conversationId: "b", seq: childSeq, parentConversationId: "wrong", items: [] });
  f.receive({ type: "opened", conversationId: "a", seq: childSeq });
  assert.equal(f.current().openId, "b");
  assert.equal(f.current().subagents, original);
  assert.equal(f.current().parentConversationId, "root");
  f.receive({ type: "subagents", conversationId: "b", seq: siblingSeq, parentConversationId: "root",
    items: [{ id: "b", title: "Updated", status: "completed" }] });
  assert.equal(f.current().subagents.length, 1);
  assert.equal(f.current().subagents[0].title, "Updated");
  f.receive({ type: "subagents", conversationId: "b", seq: siblingSeq, parentConversationId: "root", items: [] });
  assert.equal(f.current().subagents.length, 0);
});

test("unknown targets clear the family and do not rediscover it from drawer rows", () => {
  const f = fixture();
  f.evaluate('update({ conversations: [{ id: "root" }, { id: "a", parentConversationId: "root" }] })');
  const unknown = f.navigate("unknown");
  assert.equal(unknown.subagents.length, 0);
  assert.equal(unknown.parentConversationId, null);
  const previous = f.navigate("a");
  assert.equal(previous.subagents.length, 0);
  assert.equal(previous.parentConversationId, null);
});

test("New Chat and stop release retained family state", () => {
  const f = fixture();
  f.navigate("a");
  const blank = f.navigate(null);
  assert.equal(blank.openId, null);
  assert.equal(blank.parentConversationId, null);
  assert.equal(blank.subagents.length, 0);
  const g = fixture();
  g.navigate("a"); g.evaluate("sync.stop()");
  assert.equal(g.current().connected, false);
  assert.equal(g.current().connectionId, null);
  assert.equal(g.current().subagents.length, 0);
  assert.equal(g.current().parentConversationId, null);
});

test("connection identity replacement invalidates family but duplicate connection event does not", () => {
  const f = fixture(), agents = f.current().subagents;
  f.navigate("a");
  f.receive({ type: "connection", connectionId: "session" });
  assert.equal(f.current().subagents, agents);
  f.receive({ type: "connection", connectionId: "new-session" });
  assert.equal(f.current().parentConversationId, null);
  assert.equal(f.current().subagents.length, 0);
  assert.equal(f.navigate("b").subagents.length, 0);
});

test("disconnect and reconnect never restore the former family without a fresh matching event", async () => {
  const f = fixture();
  const selected = f.navigate("a"), seq = selected.switching.command.seq;
  await f.disconnect();
  assert.equal(f.current().connected, false);
  assert.equal(f.current().parentConversationId, null);
  assert.equal(f.current().subagents.length, 0);
  f.reconnect();
  f.receive({ type: "connection", connectionId: "new-session" });
  assert.equal(f.current().subagents.length, 0);
  f.receive({ type: "subagents", conversationId: "a", seq, parentConversationId: "root",
    items: [{ id: "a", title: "Restored", status: "idle" }] });
  assert.equal(f.current().parentConversationId, "root");
  assert.equal(f.current().subagents[0].title, "Restored");
});

test("family retention keeps the existing streaming patch implementation and selected-target reset", () => {
  const f = fixture();
  const selected = f.navigate("a"), seq = selected.switching.command.seq;
  f.receive({ type: "streaming", conversationId: "a", seq, message: { id: "m", text: "old" } });
  f.receive({ type: "streaming_patch", conversationId: "a", seq, messageId: "m", patch: { o: { text: { a: " new" } } } });
  assert.equal(f.current().streaming.text, "old new");
  f.navigate("b");
  assert.equal(f.current().streaming, null);
  f.receive({ type: "streaming_patch", conversationId: "a", seq, messageId: "m", patch: { o: { text: { a: " stale" } } } });
  assert.equal(f.current().streaming, null);
});

test("queue commands use the selected sequence and preserve canonical rows until acknowledgement", () => {
  const f = fixture();
  const seq = f.current().switching.command.seq;
  f.receive({ type: "composer", conversationId: "root", seq, phase: "IDLE", generating: false,
    text: "", editRevision: 0, actionId: 0, attachments: [],
    queue: [{ id: "completion", text: "Synthetic completion", attachmentCount: 0 },
      { id: "followup", text: "Synthetic follow-up", attachmentCount: 2 }] });
  const queue = f.current().composer.queue;
  f.evaluate('sync.removeQueued("completion")');
  assert.deepEqual(f.sent.at(-1), { type: "remove_queued", queuedId: "completion", seq });
  assert.equal(f.current().composer.queue, queue);
  f.evaluate("sync.submit()");
  const command = f.sent.at(-1);
  assert.equal(command.type, "send_queued");
  assert.equal(command.seq, seq);
  assert.equal(command.actionId, f.current().pendingAction);
  assert.equal(f.current().composer.queue, queue);
  const count = f.sent.length;
  f.evaluate("sync.submit()");
  assert.equal(f.sent.length, count);
  f.receive({ type: "composer", conversationId: "root", seq: seq - 1, phase: "IDLE",
    generating: false, text: "", editRevision: 0, actionId: command.actionId, attachments: [], queue: [] });
  assert.equal(f.current().composer.queue, queue);
  f.receive({ type: "composer", conversationId: "root", seq, phase: "IDLE",
    generating: false, text: "", editRevision: 0, actionId: command.actionId, attachments: [], queue: [] });
  assert.equal(f.current().composer.queue.length, 0);
  assert.equal(f.current().pendingAction, 0);
});

test("opening waits for every real initial tool snapshot in all event orders", () => {
  function permutations(items) {
    return items.length ? items.flatMap((item, index) =>
      permutations(items.filter((_, other) => other !== index)).map(rest => [item, ...rest])) : [[]];
  }
  for (const order of permutations(["path", "composer", "todo", "subagents", "interactions"])) {
    const f = fixture();
    const selected = f.navigate("unrelated");
    const seq = selected.switching.command.seq;
    const events = {
      path: { type: "path", messages: [], generating: false },
      composer: { type: "composer", phase: "IDLE", text: "", editRevision: 0, actionId: 0 },
      todo: { type: "todo", items: [] },
      subagents: { type: "subagents", items: [], parentConversationId: null },
      interactions: { type: "interactions", items: [], actionId: 0 },
    };
    for (let index = 0; index < order.length; index++) {
      f.receive({ ...events[order[index]], conversationId: "unrelated", seq });
      assert.equal(f.current().switching.phase, index === order.length - 1 ? "settling" : "loading", order.join(","));
    }
    const request = f.current().switching.id;
    f.evaluate(`sync.settled(${request}); sync.revealed(${request})`);
    f.receive({ type: "todo", conversationId: "unrelated", seq, items: [{ content: "Live item", status: "pending" }] });
    f.receive({ type: "interactions", conversationId: "unrelated", seq, actionId: 0,
      items: [{ kind: "question", id: "new", question: "Live request" }] });
    assert.equal(f.current().switching, null);
    assert.equal(f.current().todo[0].content, "Live item");
    assert.equal(f.current().interactions[0].id, "new");
  }
});

test("retained family does not satisfy the next selected owner's initial tools", () => {
  const f = fixture(), retained = f.current().subagents;
  const oldSeq = f.current().switching.command.seq;
  const selected = f.navigate("a"), seq = selected.switching.command.seq;
  f.receive({ type: "path", conversationId: "a", seq, messages: [], generating: false });
  f.receive({ type: "composer", conversationId: "a", seq, phase: "IDLE", text: "", editRevision: 0, actionId: 0 });
  f.receive({ type: "todo", conversationId: "root", seq: oldSeq, items: [] });
  f.receive({ type: "subagents", conversationId: "a", seq: oldSeq, items: [] });
  f.receive({ type: "interactions", conversationId: "a", seq: oldSeq, items: [], actionId: 0 });
  assert.equal(f.current().subagents, retained);
  assert.equal(f.current().switching.phase, "loading");
  f.receive({ type: "todo", conversationId: "a", seq, items: [] });
  f.receive({ type: "interactions", conversationId: "a", seq, items: [], actionId: 0 });
  assert.equal(f.current().switching.phase, "loading");
  f.receive({ type: "subagents", conversationId: "a", seq, parentConversationId: "root", items: retained });
  assert.equal(f.current().switching.phase, "settling");
});

test("connection replacement invalidates initial tool readiness and failures can settle", () => {
  const f = fixture(), seq = f.current().switching.command.seq;
  f.receive({ type: "todo", conversationId: "root", seq, items: [] });
  f.receive({ type: "interactions", conversationId: "root", seq, items: [], actionId: 0 });
  f.receive({ type: "connection", connectionId: "replacement" });
  f.receive({ type: "path", conversationId: "root", seq, messages: [], generating: false });
  f.receive({ type: "composer", conversationId: "root", seq, phase: "IDLE", text: "", editRevision: 0, actionId: 0 });
  assert.equal(f.current().switching.phase, "loading");
  f.receive({ type: "subagents", conversationId: "root", seq, items: [] });
  f.receive({ type: "todo", conversationId: "root", seq, items: [] });
  assert.equal(f.current().switching.phase, "loading");
  f.receive({ type: "interactions", conversationId: "root", seq, items: [], actionId: 0 });
  assert.equal(f.current().switching.phase, "settling");
  const failed = fixture();
  const failedSeq = failed.navigate("failed-owner").switching.command.seq;
  failed.receive({ type: "load_failed", conversationId: "failed-owner", seq: failedSeq });
  assert.equal(failed.current().switching.phase, "settling");
});

test("drafts, attachments and running work do not drain the queue; disconnected actions do not send", () => {
  for (const patch of [{ text: "New draft" }, { attachments: [{ id: "attachment" }] }, { generating: true }]) {
    const f = fixture(), seq = f.current().switching.command.seq;
    f.receive({ type: "composer", conversationId: "root", seq, phase: "IDLE", generating: false,
      text: "", editRevision: 0, actionId: 0, attachments: [], queue: [{ id: "queued", text: "Queued" }], ...patch });
    f.evaluate("sync.submit()");
    assert.equal(f.sent.at(-1).type, "send");
  }
  const f = fixture();
  f.evaluate("sync.stop()");
  const count = f.sent.length;
  f.evaluate('sync.removeQueued("queued"); sync.submit()');
  assert.equal(f.sent.length, count);
});
