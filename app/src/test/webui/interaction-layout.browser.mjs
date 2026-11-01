import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

// Reuse an installed Playwright package, with no dependency installation or listening server.
const { chromium } = createRequire(process.env.AGORA_PLAYWRIGHT_PACKAGE || import.meta.url)("playwright");
const assets = fileURLToPath(new URL("../../main/assets/webui/", import.meta.url));
const origin = "http://agora-layout.test";
const markup = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
  <link rel="stylesheet" href="/style.css"><link rel="stylesheet" href="/material/material.css">
  <div class="shell"><div class="chat"><div style="flex:1"></div>
  <div class="composer-host"><div id="tools"></div><div class="composer" style="height:132px"></div></div>
  </div></div>`;
const browser = await chromium.launch({ channel: "chrome", headless: true });
try {
  for (const entry of ["initial", "loading", "live"]) for (const width of [390, 1280])
    for (const reduced of [false, true]) for (const rtl of [false, true]) {
    const page = await browser.newPage({ viewport: { width, height: 900 } });
    const errors = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.route(`${origin}/**`, async route => {
      const name = new URL(route.request().url()).pathname;
      if (name === "/") return route.fulfill({ contentType: "text/html", body: markup });
      const file = path.join(assets, name);
      if (!file.startsWith(assets) || !fs.existsSync(file)) return route.fulfill({ status: 404, body: "" });
      return route.fulfill({ contentType: name.endsWith(".css") ? "text/css" : "text/javascript", body: fs.readFileSync(file) });
    });
    await page.goto(origin);
    await page.evaluate(async ({ reduced, rtl, entry }) => {
      const { render } = await import("/vendor/preact.mjs");
      const { html } = await import("/html.js");
      const { ChatToolsRow } = await import("/tools-row.js");
      const { sync } = await import("/sync.js");
      const { installTouchFeedback } = await import("/material/ripple.js");
      installTouchFeedback();
      document.documentElement.dir = rtl ? "rtl" : "ltr";
      document.querySelector(".shell").dataset.reduceMotion = String(reduced);
      window.commands = [];
      sync.interactionTarget = () => ({ connectionId: "fixture", conversationId: "root", seq: 1 });
      sync.interactionCommand = (...args) => window.commands.push(args);
      window.fixtureState = {
        connected: true, connectionId: "fixture", openId: "root", composer: { seq: 1 },
        openStatus: "ready", initialToolsReady: entry === "loading" ? 0 : 7,
        interactionPending: 0, display: { reduceMotion: reduced },
        todo: [], subagents: [], interactions: [],
      };
      window.fixtureItems = {
        todo: [{ content: "Review the layout", status: "pending" }],
        interactions: [
          { kind: "question", id: "q1", question: "Choose the layout to keep.", options: ["First option", "Second option"], allowMultiple: false },
          { kind: "question", id: "q2", question: "Any additional details?", options: [] },
        ],
      };
      window.renderFixture = patch => {
        Object.assign(window.fixtureState, patch);
        render(html`<${ChatToolsRow} state=${window.fixtureState} />`, document.querySelector("#tools"));
      };
      window.renderFixture(entry === "initial" ? window.fixtureItems : {});
    }, { reduced, rtl, entry });
    if (entry === "live") {
      await page.waitForFunction(() => document.querySelector(".tools-scroll")?.dataset.toolsOpening === "false");
      await page.evaluate(() => window.renderFixture(window.fixtureItems));
    } else if (entry === "loading") {
      // Match sync.js's Subagent/Todo/Interaction readiness bits, without a confirmed-empty shortcut.
      for (const bit of [1, 2, 4]) {
        await page.evaluate(bit => window.renderFixture({
          initialToolsReady: window.fixtureState.initialToolsReady | bit,
          ...(bit === 2 ? { todo: window.fixtureItems.todo } : {}),
          ...(bit === 4 ? { interactions: window.fixtureItems.interactions } : {}),
        }), bit);
      }
    }
    await page.waitForFunction(() => document.querySelector(".tools-scroll")?.dataset.toolsEnter === "true" &&
      +getComputedStyle(document.querySelector(".tools-scroll")).opacity > .999 &&
      +getComputedStyle(document.querySelector(".interaction-card")).opacity > .999);
    await page.waitForTimeout(400);
    const geometry = () => page.evaluate(() => {
      const rect = selector => {
        const node = document.querySelector(selector), box = node.getBoundingClientRect();
        return { x: box.x, y: box.y, width: box.width, height: box.height, right: box.right, bottom: box.bottom };
      };
      const selectors = [".tools-scroll", ".interaction-host", ".interaction-card", ".interaction-header",
        ".interaction-minimize", ".interaction-minimize svg", ".interaction-position", ".interaction-actions",
        ".interaction-actions .filled-button", ".todo-capsule", ".tools-scrollbar", ".composer"];
      const boxes = Object.fromEntries(selectors.map(selector => [selector, rect(selector)]));
      const button = document.querySelector(".interaction-minimize"), bounds = button.getBoundingClientRect();
      return { boxes, collapseHit: document.elementFromPoint(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2)?.closest("button") === button,
        lift: parseFloat(document.querySelector(".chat").style.getPropertyValue("--chat-tools-lift")),
        scrollbarOpacity: getComputedStyle(document.querySelector(".tools-scrollbar")).opacity,
        expanded: document.querySelector(".tools-scroll").dataset.toolsExpanded,
        documentOverflow: document.documentElement.scrollWidth > innerWidth };
    });
    const expanded = await geometry();
    const b = expanded.boxes, card = b[".interaction-card"], header = b[".interaction-header"];
    const collapse = b[".interaction-minimize"], action = b[".interaction-actions .filled-button"];
    const viewport = b[".tools-scroll"], todo = b[".todo-capsule"], position = b[".interaction-position"];
    console.log(JSON.stringify({ entry, width, reduced, rtl, expanded }));
    assert.equal(expanded.collapseHit, true);
    assert.equal(expanded.documentOverflow, false);
    assert.equal(expanded.expanded, "true");
    assert.equal(expanded.scrollbarOpacity, "0");
    assert.equal(card.width, b[".interaction-host"].width);
    assert.equal(card.width, viewport.width - 24);
    assert.ok(Math.abs(card.x - viewport.x - 12) < .5);
    assert.ok(Math.abs(viewport.right - card.right - 12) < .5);
    assert.ok(Math.abs((rtl ? card.x - todo.right : todo.x - card.right) - 8) < .5);
    assert.equal(todo.bottom, card.bottom);
    assert.equal(b[".composer"].y - card.bottom, 8);
    assert.equal(collapse.width, 48);
    assert.equal(collapse.height, 48);
    assert.equal(b[".interaction-minimize svg"].width, 20);
    assert.ok(Math.abs(collapse.y + collapse.height / 2 - header.y - header.height / 2) < .5);
    assert.equal(rtl ? collapse.x - card.x : card.right - collapse.right, 4);
    assert.ok(rtl ? position.x >= collapse.right : position.right <= collapse.x);
    assert.ok(action.x >= card.x + 15 && action.right <= card.right - 15);
    assert.equal(card.bottom - action.bottom, 14);
    const noSliver = await page.evaluate(() => {
      const todo = document.querySelector(".todo-capsule"), box = todo.getBoundingClientRect();
      const viewport = document.querySelector(".tools-scroll").getBoundingClientRect();
      const x = document.dir === "rtl" ? viewport.x + 2 : viewport.right - 2;
      const card = document.querySelector(".interaction-card"), cardBox = card.getBoundingClientRect();
      return { hit: document.elementFromPoint(x, box.y + box.height / 2)?.closest(".todo-capsule") === todo,
        editorClip: getComputedStyle(card).clipPath, editorRadius: getComputedStyle(card).borderTopLeftRadius,
        editorShadow: getComputedStyle(card).boxShadow,
        cornerHit: document.elementFromPoint(cardBox.x + 1, cardBox.y + 1)?.closest(".interaction-card") === card };
    });
    assert.equal(noSliver.hit, false);
    assert.equal(noSliver.editorClip, "none");
    assert.equal(noSliver.editorRadius, "24px");
    assert.notEqual(noSliver.editorShadow, "none");
    assert.equal(noSliver.cornerHit, false, "the card still clips its rounded top corner");
    const strip = { x: rtl ? viewport.x : viewport.right - 4, y: todo.y, width: 4, height: todo.height };
    const clippedPaint = await page.screenshot({ clip: strip });
    await page.locator("[data-tools-key='todo'] > .tools-item-visual").evaluate(node => {
      window.savedTodoPaint = { opacity: node.style.opacity, clip: node.style.clipPath };
      node.style.opacity = "0";
    });
    const absentPaint = await page.screenshot({ clip: strip });
    assert.deepEqual(clippedPaint, absentPaint, "the entire outer 4px strip must match absent Todo paint");
    await page.locator("[data-tools-key='todo'] > .tools-item-visual").evaluate(node => {
      node.style.opacity = window.savedTodoPaint.opacity;
      node.style.clipPath = "none";
    });
    const unboundedPaint = await page.screenshot({ clip: strip });
    assert.notDeepEqual(unboundedPaint, absentPaint, "the paint assertion must detect the original sliver");
    await page.locator("[data-tools-key='todo'] > .tools-item-visual").evaluate(node => {
      node.style.clipPath = window.savedTodoPaint.clip;
    });

    await page.locator(".interaction-option").first().click();
    await page.locator(".interaction-actions .filled-button").click();
    await page.waitForFunction(() => document.querySelector(".interaction-question")?.textContent === "Any additional details?");
    await page.locator(".interaction-body:not(.interaction-outgoing) textarea").fill("Retain this draft");
    await page.waitForTimeout(400);
    const samples = await page.evaluate(async () => {
      const frames = [];
      const start = performance.now();
      const sample = () => {
        const card = document.querySelector(".interaction-card").getBoundingClientRect();
        const todo = document.querySelector(".todo-capsule").getBoundingClientRect();
        const host = document.querySelector(".interaction-host").getBoundingClientRect();
        frames.push({ gap: document.dir === "rtl" ? card.x - todo.right : todo.x - card.right,
          widthError: Math.abs(host.width - card.width) });
      };
      document.querySelector(".interaction-minimize").click();
      await new Promise(resolve => {
        function tick(now) { sample(); if (now - start < 400) requestAnimationFrame(tick); else resolve(); }
        requestAnimationFrame(tick);
      });
      return frames;
    });
    assert.ok(samples.length > 1);
    for (const sample of samples) {
      assert.ok(sample.gap >= 7, `fold gap ${sample.gap}`);
      assert.ok(sample.widthError < 1.1, `fold slot/card mismatch ${sample.widthError}`);
    }
    assert.equal(await page.locator(".interaction-card").evaluate(node => Math.round(node.getBoundingClientRect().height)), 48);
    assert.equal(await page.locator(".tools-scroll").getAttribute("data-tools-expanded"), "false");
    assert.equal(await page.locator("[data-tools-key='todo'] > .tools-item-visual").evaluate(node => node.style.clipPath), "");
    await page.locator(".todo-capsule").click();
    await page.waitForFunction(() => document.querySelector(".detail-sheet"));
    await page.keyboard.press("Escape");
    await page.waitForFunction(() => !document.querySelector(".detail-sheet-layer"));
    await page.locator(".interaction-capsule").click();
    await page.waitForTimeout(400);
    assert.equal(await page.locator(".interaction-question").textContent(), "Any additional details?");
    assert.equal(await page.locator(".interaction-body:not(.interaction-outgoing) textarea").inputValue(), "Retain this draft");
    assert.equal((await geometry()).collapseHit, true);
    await page.locator(".interaction-actions .filled-button").click();
    const command = await page.evaluate(() => window.commands.at(-1));
    assert.equal(command[0], "question_submit");
    assert.deepEqual(command[1].answers.q1.choices, ["First option"]);
    assert.equal(command[1].answers.q2.text, "Retain this draft");
    await page.evaluate(() => window.renderFixture({ interactions: [], interactionPending: 1 }));
    assert.equal(await page.locator(".interaction-question").textContent(), "Any additional details?");
    await page.waitForFunction(() => !document.querySelector(".interaction-card"));
    await page.waitForTimeout(200);
    assert.equal(await page.locator(".todo-capsule").count(), 1);
    await page.evaluate(() => window.renderFixture({ todo: [] }));
    await page.waitForFunction(() => !document.querySelector(".todo-capsule"));
    await page.waitForTimeout(200);
    assert.equal(await page.locator(".chat").evaluate(node => node.style.getPropertyValue("--chat-tools-lift")), "0px");
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({ entry, width, reduced, rtl, cardWidth: card.width,
      outerInset: card.x - viewport.x, itemGap: rtl ? card.x - todo.right : todo.x - card.right,
      collapseEndInset: rtl ? collapse.x - card.x : card.right - collapse.right,
      actionBottomInset: card.bottom - action.bottom, foldFrames: samples.length, passed: true }));
    await page.close();
  }
} finally {
  await browser.close();
}
