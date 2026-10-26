// End-to-end: the JavaScript client (packages/webi/client/webi.js) against a
// REAL Kosh webi server over real sockets (tests/js/e2e_server.kh).
"use strict";
const { spawn } = require("child_process");
const path = require("path");
const net = require("net");
const Webi = require("../../packages/webi/client/webi.js");

const ROOT = path.join(__dirname, "..", "..");
const KOSH = process.env.KOSH || path.join(ROOT, process.platform === "win32" ? "kosh.exe" : "kosh");
let pass = 0, fail = 0;
function eq(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log("  ✗  FAIL  " + name + "\n       got  " + JSON.stringify(got) + "\n       want " + JSON.stringify(want)); }
}
const ok = (name, c) => eq(name, !!c, true);

function freePort() {
  return new Promise((res, rej) => { const s = net.createServer(); s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); }); s.on("error", rej); });
}
async function waitUp(base, ms) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { try { await fetch(base + "/"); return true; } catch (e) { await new Promise(r => setTimeout(r, 100)); } }
  return false;
}

(async () => {
  const port = await freePort();
  const base = "http://127.0.0.1:" + port;
  const srv = spawn(KOSH, [path.join(__dirname, "e2e_server.kh"), String(port)], { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });
  let log = ""; srv.stdout.on("data", d => log += d); srv.stderr.on("data", d => log += d);
  const stop = () => { try { srv.kill("SIGKILL"); } catch (e) {} };
  process.on("exit", stop);
  if (!(await waitUp(base, 8000))) { console.log("server did not start:\n" + log); stop(); process.exit(2); }

  try {
    // ---- plain access rules ------------------------------------------------------------
    let r = await fetch(base + "/");
    eq("public home is served in the clear", r.status, 200);
    r = await fetch(base + "/hello");
    eq("plain request to a private route is 426", r.status, 426);
    ok("and leaks nothing", !(await r.text()).includes("Hello"));
    r = await fetch(base + "/blog/post", { headers: { "User-Agent": "Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)" } });
    eq("spoofed Googlebot from loopback gets 426 on a bot path", r.status, 426);
    r = await fetch(base + "/blog/post");
    eq("plain visitor gets 426 on a bot path", r.status, 426);

    // ---- channel -----------------------------------------------------------------------
    const ch = await Webi.connect(base);
    ok("handshake gives a 32-hex session id", /^[0-9a-f]{32}$/.test(ch.session.sid));

    let a = await ch.request("GET", "/hello");
    eq("encrypted GET status", a.status, 200);
    eq("encrypted GET body", a.body, "<h1>Hello secret world</h1>");
    ok("content type survives", /html/.test(a.contentType));

    a = await ch.request("GET", "/blog/post");
    eq("bot path readable through the channel", a.body, "<h1>blog post</h1>");

    const uni = "héllo wörld ✓ 你好 😀 \"quoted\" back\\slash\ttab";
    a = await ch.request("POST", "/echo?x=42", { body: uni, query: "y=7" });
    const echoed = JSON.parse(a.body);
    eq("unicode body round-trips through JSON/UTF-8", echoed.body, uni);
    eq("method delivered", echoed.method, "POST");
    eq("query from the path is delivered", echoed.q, "42");
    eq("opts.query is merged with the query in the path", echoed.ua, "7");
    eq("response headers survive", a.headers["X-Echo"], "yes");

    a = await ch.request("GET", "/headers", { headers: { "X-Custom": "from-js" } });
    eq("request headers are delivered", a.body, "from-js");
    a = await ch.request("GET", "/cookie");
    eq("cookies are returned", a.cookies.length, 1);
    a = await ch.request("GET", "/nope");
    eq("sealed 404", a.status, 404);
    a = await ch.request("TRACE", "/hello");
    eq("unsupported method gets a sealed 400", a.status, 400);
    a = await ch.request("GET", "/_webi/handshake");
    eq("nested /_webi/ path gets a sealed 400", a.status, 400);

    const big = "0123456789abcdef".repeat(8192);  // 128 KB
    a = await ch.request("POST", "/big", { body: big });
    eq("128 KB body round-trips", a.body === big, true);

    // many sequential and parallel requests exercise counters and the replay window
    for (let i = 0; i < 100; i++) { a = await ch.request("GET", "/hello"); if (a.status !== 200) break; }
    eq("100 sequential requests all succeed", a.status, 200);
    const par = await Promise.all(Array.from({ length: 20 }, () => ch.request("GET", "/hello")));
    eq("20 parallel requests all succeed", par.every(x => x.status === 200), true);
    eq("send counter advanced as expected", ch.session.sendCtr >= 125, true);

    // ---- attacks on the wire -----------------------------------------------------------------
    const raw = async (packet) => fetch(base + "/_webi/secure", { method: "POST", body: packet });
    const pkt = ch.session.seal("webi-req", JSON.stringify({ method: "GET", path: "/hello", query: "", headers: "", body: "" }));
    let w1 = await raw(pkt);
    eq("first delivery of a captured packet works", w1.status, 200);
    let w2 = await raw(pkt);
    eq("replay of the same packet is 403", w2.status, 403);
    eq("with the generic body", await w2.text(), "rejected");
    const pkt2 = ch.session.seal("webi-req", JSON.stringify({ method: "GET", path: "/hello", query: "", headers: "", body: "" }));
    const flipped = pkt2.slice(0, -1) + (pkt2.slice(-1) === "0" ? "1" : "0");
    eq("tampered packet is 403", (await raw(flipped)).status, 403);
    eq("the genuine packet still works afterwards", (await raw(pkt2)).status, 200);
    eq("garbage is 403", (await raw("not a packet")).status, 403);

    // ---- a second session is independent -------------------------------------------------------------
    const ch2 = await Webi.connect(base);
    ok("second session has its own id", ch2.session.sid !== ch.session.sid);
    eq("second session works", (await ch2.request("GET", "/hello")).body, "<h1>Hello secret world</h1>");
    const stolen = ch.session.seal("webi-req", "{}").replace(ch.session.sid, ch2.session.sid);
    eq("a packet re-addressed to another session is 403", (await raw(stolen)).status, 403);

    // handshake abuse
    const hs = async (body) => fetch(base + "/_webi/handshake", { method: "POST", body, headers: { "Content-Type": "application/json" } });
    eq("handshake with a low-order key is 400", (await hs(JSON.stringify({ v: 1, pub: "00".repeat(32) }))).status, 400);
    eq("handshake with a bad version is 400", (await hs(JSON.stringify({ v: 9, pub: "00" }))).status, 400);
  } catch (e) {
    fail++; console.log("  ✗  EXCEPTION " + (e && e.stack || e));
  }
  stop();
  console.log("  " + pass + "/" + (pass + fail) + " passed   |   " + fail + " failed");
  if (fail) console.log("--- server log ---\n" + log.slice(-1500));
  process.exit(fail ? 1 : 0);
})();
