// Browser-like test: the shell page + client.js bootstrap inside jsdom,
// against a REAL Kosh server. Needs the `jsdom` npm package; skipped with
// exit code 0 and a message if it is not installed (CI installs it).
"use strict";
let JSDOM;
try { ({ JSDOM } = require(process.env.JSDOM_PATH || "jsdom")); }
catch (e) { console.log("  jsdom not installed - browser test SKIPPED"); process.exit(0); }
const { spawn } = require("child_process");
const path = require("path");
const net = require("net");
const crypto = require("crypto");

const ROOT = path.join(__dirname, "..", "..");
const KOSH = process.env.KOSH || path.join(ROOT, process.platform === "win32" ? "kosh.exe" : "kosh");
let pass = 0, fail = 0;
function eq(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log("  ✗  FAIL  " + name + "\n       got  " + JSON.stringify(got) + "\n       want " + JSON.stringify(want)); }
}
const freePort = () => new Promise((res, rej) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); s.on("error", rej); });
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
async function waitUp(base, ms) { const t0 = Date.now(); while (Date.now() - t0 < ms) { try { await fetch(base + "/"); return true; } catch (e) { await sleep(100); } } return false; }

// Opens `url` the way a browser would: GET with Accept: text/html, run the
// returned HTML (the shell) with scripts enabled, and wait for the result.
async function browse(base, urlPath, until) {
  const resp = await fetch(base + urlPath, { headers: { Accept: "text/html,application/xhtml+xml" } });
  const html = await resp.text();
  const dom = new JSDOM(html, {
    url: base + urlPath, runScripts: "dangerously", resources: "usable", pretendToBeVisual: true,
    beforeParse(w) {
      w.fetch = (u, o) => fetch(new URL(u, base + urlPath), o);
      w.TextEncoder = TextEncoder; w.TextDecoder = TextDecoder;
      Object.defineProperty(w, "crypto", { value: crypto.webcrypto, configurable: true });
    }
  });
  const t0 = Date.now();
  while (Date.now() - t0 < 8000) {
    if (until(dom.window.document)) break;
    await sleep(50);
  }
  return { status: resp.status, shell: html, dom };
}

async function startServer(extra) {
  const port = await freePort(), base = "http://127.0.0.1:" + port;
  const srv = spawn(KOSH, [path.join(__dirname, "e2e_server.kh"), String(port)].concat(extra || []), { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });
  const h = { base, srv, log: "" };
  srv.stdout.on("data", d => h.log += d); srv.stderr.on("data", d => h.log += d);
  h.stop = () => { try { srv.kill("SIGKILL"); } catch (e) {} };
  process.on("exit", h.stop);
  if (!(await waitUp(base, 8000))) { console.log("server did not start:\n" + h.log); h.stop(); process.exit(2); }
  return h;
}

// Runs /_webi/proof in jsdom; returns [{cls, name}] for every check shown.
async function runProof(base) {
  const dom = new JSDOM(await (await fetch(base + "/_webi/proof")).text(), {
    url: base + "/_webi/proof", runScripts: "dangerously", resources: "usable", pretendToBeVisual: true,
    beforeParse(w) {
      w.fetch = (u, o) => fetch(new URL(u, base + "/_webi/proof"), o);
      w.TextEncoder = TextEncoder; w.TextDecoder = TextDecoder;
      Object.defineProperty(w, "crypto", { value: crypto.webcrypto, configurable: true });
    }
  });
  const t0 = Date.now();
  while (Date.now() - t0 < 15000 && !dom.window.document.documentElement.hasAttribute("data-proof-done")) await sleep(50);
  const d = dom.window.document;
  const rows = Array.from(d.querySelectorAll("#checks li")).map(li => ({ cls: li.className, name: li.querySelector("strong").textContent.trim() }));
  const out = { rows, verdict: d.documentElement.getAttribute("data-proof-done"), summary: d.getElementById("summary").textContent, facts: d.getElementById("facts").textContent };
  dom.window.close();
  return out;
}

(async () => {
  const S = await startServer();
  const base = S.base, log = () => S.log, stop = S.stop;
  try {
    const js = await fetch(base + "/_webi/client.js");
    eq("client.js is served", js.status, 200);
    eq("with a JavaScript content type", /javascript/.test(js.headers.get("content-type")), true);
    const body = await js.text();
    eq("and its hash header matches the bytes", crypto.createHash("sha256").update(body).digest("hex"), js.headers.get("x-webi-client-sha256"));

    const r = await browse(base, "/page?n=hello42", d => d.getElementById("t") && d.getElementById("t").textContent.includes("decrypted"));
    eq("the browser first got the shell (HTTP 200)", r.status, 200);
    eq("the shell contains none of the real content", r.shell.includes("decrypted page"), false);
    eq("the shell never mentions the query value", r.shell.includes("hello42"), false);
    const d = r.dom.window.document;
    eq("the document was replaced with the decrypted page", d.getElementById("t") && d.getElementById("t").textContent, "decrypted page");
    eq("the query string went through the channel", d.getElementById("q") && d.getElementById("q").textContent, "hello42");
    eq("the page's own title came through", d.title, "Secret page");
    eq("scripts inside the decrypted page run (document.write semantics)", d.getElementById("t") && d.getElementById("t").getAttribute("data-ran"), "yes");
    r.dom.window.close();

    const e = await browse(base, "/nope-not-here", d => d.body && d.body.textContent.includes("404"));
    const eb = e.dom.window.document.body.textContent;
    eq("an unknown route is shown from the sealed 404 (not a transport error)", /404|Not Found/i.test(eb), true);
    e.dom.window.close();

    const j = await fetch(base + "/page", { headers: { Accept: "application/json" } });
    eq("a non-browser client still gets 426", j.status, 426);

    // ---- the live proof page on a healthy server -------------------------------------------------
    const pr = await runProof(base);
    eq("proof page finished", pr.verdict, "pass");
    eq("no check failed", pr.rows.filter(r => r.cls === "fail").length, 0);
    eq("at least 8 checks passed", pr.rows.filter(r => r.cls === "pass").length >= 8, true);
    eq("it warns that this test run is not on HTTPS", pr.rows.some(r => r.cls === "warn" && /HTTPS/.test(r.name)), true);
    eq("replay check ran and passed", pr.rows.some(r => r.cls === "pass" && /Replaying/.test(r.name)), true);
    eq("tamper check ran and passed", pr.rows.some(r => r.cls === "pass" && /flipped bit/.test(r.name)), true);
    eq("it admits there has been no external review", /NO - not reviewed/.test(pr.facts), true);
    eq("summary mentions the warning", /1 warnings/.test(pr.summary), true);
    const pj = await (await fetch(base + "/_webi/proof.json")).json();
    eq("server counted the proof page's replay/tamper attempts", pj.stats.secure_requests_rejected >= 2, true);
  } catch (e) { fail++; console.log("  ✗  EXCEPTION " + (e && e.stack || e)); }
  stop();

  // ---- the proof page must FAIL on a server with replay protection disabled -----------------------------
  try {
    const B = await startServer(["broken"]);
    const br = await runProof(B.base);
    eq("broken server: proof page reports failure", br.verdict, "fail");
    eq("broken server: the replay check is the one that fails", br.rows.some(r => r.cls === "fail" && /Replay/i.test(r.name)), true);
    eq("broken server: the summary says so", /[1-9][0-9]* failed/.test(br.summary), true);
    B.stop();
  } catch (e) { fail++; console.log("  ✗  EXCEPTION (broken-server run) " + (e && e.stack || e)); }

  console.log("  " + pass + "/" + (pass + fail) + " passed   |   " + fail + " failed");
  if (fail) console.log("--- server log ---\n" + log().slice(-1200));
  process.exit(fail ? 1 : 0);
})();
