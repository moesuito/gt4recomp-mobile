/*
 * TASK-185 dev runner (Node + Playwright, development only; not part of the launcher build).
 *
 *   node ui/dev/t185-checks.js checks     the focus checks in en-US and pt-BR, then nav-test; exits 1 on a failure
 *   node ui/dev/t185-checks.js shots      the eight screenshots in build/evidence/t185-editor/
 *
 * Serves ui/ over http://127.0.0.1:8765 (the pages fetch their locales and assets with relative URLs). Playwright is
 * the machine-wide install (NODE_PATH=%APPDATA%\npm\node_modules); no package is added to the repository.
 */
"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");

const { chromium } = require("playwright");

const UI_DIR = path.resolve(__dirname, "..");
const OUT_DIR = path.resolve(__dirname, "../../../../../build/evidence/t185-editor");
const PORT = 0; // any free port: 8765 is not always free on the development machine
const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".png": "image/png",
  ".webp": "image/webp",
  ".ttf": "font/ttf",
  ".svg": "image/svg+xml"
};

function serve() {
  const server = http.createServer((request, response) => {
    let requestPath = decodeURIComponent(request.url.split("?")[0].split("#")[0]);
    if (requestPath === "/" || requestPath === "") {
      requestPath = "/index.html";
    }
    const file = path.join(UI_DIR, requestPath);
    if (!file.startsWith(UI_DIR)) {
      response.writeHead(403);
      response.end();
      return;
    }
    fs.readFile(file, (error, data) => {
      if (error) {
        response.writeHead(404);
        response.end("not found");
        return;
      }
      response.writeHead(200, { "Content-Type": MIME[path.extname(file).toLowerCase()] || "application/octet-stream" });
      response.end(data);
    });
  });
  return new Promise((resolve) => {
    server.listen(PORT, "127.0.0.1", () => resolve(server));
  });
}

function baseUrl() {
  return `http://127.0.0.1:${actualPort}`;
}

let actualPort = PORT;

async function focusChecks(browser, language) {
  const page = await browser.newPage({ viewport: { width: 1366, height: 768 } });
  const url = `${baseUrl()}/dev/focus-check.html?lang=${language}`;
  await page.goto(url, { waitUntil: "load" });
  await page.waitForFunction(() => document.body.getAttribute("data-focus-check"), null, { timeout: 240000 });
  const text = await page.locator("#focus-check-results").textContent();
  const verdict = await page.getAttribute("body", "data-focus-check");
  await page.close();
  const lines = String(text || "").split("\n");
  const summary = lines.shift() || "";
  const failures = lines.filter((line) => line.startsWith("FAIL"));
  return { language, summary, failures, verdict, tail: lines.slice(-6) };
}

async function navTest(browser) {
  const page = await browser.newPage({ viewport: { width: 1366, height: 768 } });
  await page.goto(`${baseUrl()}/dev/nav-test.html`, { waitUntil: "load" });
  await page.waitForFunction(() => window.GT4_NAV_TEST_RESULT, null, { timeout: 60000 });
  const result = await page.evaluate(() => window.GT4_NAV_TEST_RESULT);
  const lines = await page.locator("#results li").allTextContents();
  await page.close();
  return { result, failures: lines.filter((line) => line.startsWith("FAIL")) };
}

async function shots(browser) {
  fs.mkdirSync(OUT_DIR, { recursive: true });
  const sizes = [
    { label: "2400x1080", width: 2400, height: 1080 },
    { label: "1366x768", width: 1366, height: 768 }
  ];
  const scenarios = [
    { scenario: "vpad-controls", name: "controls-vpad", wait: ".cx-vpad" },
    { scenario: "vpad-editor", name: "editor", wait: ".vp-editor .vp-el.is-selected" }
  ];
  for (const size of sizes) {
    for (const language of ["en-US", "pt-BR"]) {
      for (const item of scenarios) {
        const page = await browser.newPage({ viewport: { width: size.width, height: size.height } });
        await page.goto(`${baseUrl()}/dev/focus-check.html?lang=${language}&scenario=${item.scenario}`, { waitUntil: "load" });
        await page.waitForSelector(item.wait, { timeout: 30000 });
        await page.waitForTimeout(900);
        const file = path.join(OUT_DIR, `${item.name}-${language}-${size.label}.png`);
        await page.screenshot({ path: file });
        console.log(`shot ${file}`);
        await page.close();
      }
    }
  }
}

async function main() {
  const mode = process.argv[2] || "checks";
  const server = await serve();
  actualPort = server.address().port;
  const browser = await chromium.launch();
  let failed = false;
  try {
    if (mode === "shots") {
      await shots(browser);
    } else {
      for (const language of ["en-US", "pt-BR"]) {
        const result = await focusChecks(browser, language);
        console.log(`focus ${language}: ${result.summary} [${result.verdict}]`);
        result.failures.forEach((line) => console.log("  " + line));
        result.tail.forEach((line) => console.log("  | " + line));
        failed = failed || result.verdict !== "pass";
      }
      const nav = await navTest(browser);
      console.log(`nav-test: ${nav.result.passed} of ${nav.result.total} passed`);
      nav.failures.forEach((line) => console.log("  " + line));
      failed = failed || nav.result.passed !== nav.result.total;
    }
  } finally {
    await browser.close();
    server.close();
  }
  process.exit(failed ? 1 : 0);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
