"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const { openReleaseFeatures, followReleaseFeaturesLink } = require("../docs/_javascript/release_features.js");

const page = "https://mikecarper.github.io/MeshCore/releases/1.18.0.1/";
const script = fs.readFileSync(require.resolve("../docs/_javascript/release_features.js"), "utf8");

function fixture(hash = "", readyState = "complete") {
  const documentHandlers = new Map();
  const windowHandlers = new Map();
  let navigation;
  let element;
  const location = { href: page + hash, hash };
  const doc = {
    baseURI: page,
    readyState,
    getElementById(id) {
      assert.equal(id, "extra-features");
      return element;
    },
    addEventListener(name, callback, options) {
      if (name === "DOMContentLoaded") assert.equal(options.once, true);
      assert.equal(documentHandlers.has(name), false, "handlers are installed once per page");
      documentHandlers.set(name, callback);
    }
  };
  function replaceDetails(tagName = "DETAILS") {
    element = { tagName, open: false, scrolls: 0, scrollIntoView(options) {
      assert.equal(this.open, true, "expand before scrolling");
      assert.equal(options.block, "start");
      this.scrolls++;
    } };
    return element;
  }
  replaceDetails();
  return {
    doc, location, replaceDetails,
    get details() { return element; },
    removeDetails() { element = null; },
    boot() {
      vm.runInNewContext(script, {
        URL,
        window: { location, addEventListener(name, callback) { windowHandlers.set(name, callback); } },
        document: doc,
        document$: { subscribe(callback) { navigation = callback; } }
      });
    },
    ready() { documentHandlers.get("DOMContentLoaded")(); },
    navigate(hash) { location.hash = hash; location.href = page + hash; windowHandlers.get("hashchange")(); },
    materialNavigate() { navigation(); },
    click(event) { documentHandlers.get("click")(event); }
  };
}

function click(href, properties = {}, attributes = {}) {
  const values = { href, ...attributes };
  const link = {
    getAttribute(name) { return values[name] ?? null; },
    hasAttribute(name) { return Object.hasOwn(values, name); }
  };
  return {
    button: 0,
    target: { closest(selector) { assert.equal(selector, "a[href]"); return link; } },
    preventDefault() { assert.fail("normal link navigation must be preserved"); },
    ...properties
  };
}

// An initial fragment must work both before and after DOM readiness.
const loading = fixture("#extra-features", "loading");
loading.boot();
assert.equal(loading.details.open, false);
loading.ready();
assert.equal(loading.details.open, true);
assert.equal(loading.details.scrolls, 1);
const ready = fixture("#extra-features");
ready.boot();
assert.equal(ready.details.open, true);

// Unrelated hashes do not expand, including after an instant-navigation update.
const ordinary = fixture("#install");
ordinary.boot();
ordinary.materialNavigate();
assert.equal(ordinary.details.open, false);
assert.equal(ordinary.details.scrolls, 0);
ordinary.navigate("#extra-features");
assert.equal(ordinary.details.open, true);
const replacement = ordinary.replaceDetails();
ordinary.materialNavigate();
assert.equal(replacement.open, true);
assert.equal(replacement.scrolls, 1);

// Clicking the already-selected fragment reopens a manually closed list.
replacement.open = false;
ordinary.click(click("#extra-features"));
assert.equal(replacement.open, true);
assert.equal(replacement.scrolls, 2);
replacement.open = false;
ordinary.click(click(page + "#extra-features"));
assert.equal(replacement.open, true);

// Same-page clicks work without depending on a future hashchange event.
const directClick = fixture();
directClick.boot();
directClick.click(click("#extra-features"));
assert.equal(directClick.details.open, true);
assert.equal(directClick.location.hash, "", "the browser owns URL navigation");

// Preserve normal browser actions and links to other pages or sites.
const rejected = [
  click("#install"),
  click("#extra-features", { defaultPrevented: true }),
  ...["ctrlKey", "metaKey", "shiftKey", "altKey"].map(key => click("#extra-features", { [key]: true })),
  click("#extra-features", { button: 1 }),
  click("#extra-features", { button: 2 }),
  click("#extra-features", {}, { target: "_blank" }),
  click("#extra-features", {}, { target: "another-window" }),
  click("#extra-features", {}, { download: "features.html" }),
  click("https://example.org/MeshCore/releases/1.18.0.1/#extra-features"),
  click("../1.18.0/#extra-features"),
  click("?different=1#extra-features"),
  click("mailto:example@example.org#extra-features"),
  { button: 0, target: null },
  { button: 0, target: { closest() { return null; } } }
];
for (const event of rejected) {
  const node = ordinary.replaceDetails();
  assert.equal(followReleaseFeaturesLink(event, ordinary.doc, ordinary.location), false);
  assert.equal(node.open, false);
  assert.equal(node.scrolls, 0);
}

// Text nodes inside links, explicit same-frame targets and a base URL are supported.
const textClick = click("#extra-features", {}, { target: "_self" });
textClick.target = { parentElement: textClick.target };
assert.equal(followReleaseFeaturesLink(textClick, ordinary.doc, ordinary.location), true);
ordinary.doc.baseURI = "https://example.org/";
const untouched = ordinary.replaceDetails();
assert.equal(followReleaseFeaturesLink(click("#extra-features"), ordinary.doc, ordinary.location), false);
assert.equal(untouched.open, false);

// Missing sections and an accidentally reused ID are harmless.
ordinary.removeDetails();
assert.equal(openReleaseFeatures(ordinary.doc, "#extra-features"), false);
ordinary.replaceDetails("DIV");
assert.equal(openReleaseFeatures(ordinary.doc, "#extra-features"), false);
assert.equal(openReleaseFeatures(ordinary.doc, "#extra-features-other"), false);

console.log("Release feature links: startup, hash, repeat click, navigation and browser-action guards passed.");
