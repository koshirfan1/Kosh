// Fuzz + differential tests for the JavaScript client.
//   node tests/js/fuzz_webi_js.js [seed] [scale]
// Deterministic for a given seed. Parts:
//   A. crypto differential vs Node's OpenSSL on random inputs
//   B. Session.open: garbage and EVERY single-character mutation of a valid
//      packet must be rejected
//   C. JS client <-> real Kosh server with random unicode bodies (JSON/UTF-8
//      compatibility, a classic source of bugs)
"use strict";
const crypto = require("crypto");
const { spawn } = require("child_process");
const net = require("net");
const path = require("path");
const Webi = require("../../packages/webi/client/webi.js");
const I = Webi._internal;

const SEED = Number(process.argv[2] || 1) >>> 0;
const SCALE = Number(process.argv[3] || 1);
let s = SEED || 1;
function rnd() {                       // mulberry32
  s = (s + 0x6D2B79F5) >>> 0;
  let t = s;
  t = Math.imul(t ^ (t >>> 15), t | 1);
  t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const ri = (n) => Math.floor(rnd() * n);
const rb = (n) => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = ri(256); return b; };

let pass = 0, fail = 0;
function check(name, cond, extra) {
  if (cond) pass++;
  else { fail++; if (fail <= 15) console.log("  ✗  FAIL  " + name + (extra ? "\n       " + extra : "") + "   (seed=" + SEED + ")"); }
}
const H = (b) => I.bytesToHex(b);

// ---------------------------------------------------------------- A. differential
for (let n = 0; n < 400 * SCALE; n++) {
  const len = rnd() < 0.8 ? ri(600) : ri(6000), alen = ri(80);
  const key = rb(32), nonce = rb(12), aad = rb(alen), pt = rb(len);
  const c = crypto.createCipheriv("chacha20-poly1305", key, nonce, { authTagLength: 16 });
  c.setAAD(aad, { plaintextLength: len });
  const want = Buffer.concat([c.update(pt), c.final(), c.getAuthTag()]);
  const got = I.aeadEncrypt(key, nonce, aad, pt);
  check("AEAD encrypt differs from OpenSSL (len=" + len + ", aad=" + alen + ")", Buffer.from(got).equals(want));
  const back = I.aeadDecrypt(key, nonce, aad, want);
  check("AEAD decrypt of OpenSSL output failed (len=" + len + ")", back !== null && Buffer.from(back).equals(pt));
  if (len > 0) {                              // random single-bit flip must be rejected
    const bad = Uint8Array.from(want); bad[ri(bad.length)] ^= 1 << ri(8);
    check("flipped AEAD bit accepted", I.aeadDecrypt(key, nonce, aad, bad) === null);
  }
}
for (let n = 0; n < 60 * SCALE; n++) {
  const a = crypto.generateKeyPairSync("x25519"), b = crypto.generateKeyPairSync("x25519");
  const aPriv = a.privateKey.export({ type: "pkcs8", format: "der" }).subarray(-32);
  const bPub = b.publicKey.export({ type: "spki", format: "der" }).subarray(-32);
  const want = crypto.diffieHellman({ privateKey: a.privateKey, publicKey: b.publicKey });
  check("X25519 shared differs from OpenSSL", Buffer.from(I.x25519Shared(aPriv, bPub)).equals(want));
}
for (let n = 0; n < 60 * SCALE; n++) {                  // arbitrary 32-byte scalars/points (clamping, high bit)
  const k = rb(32), u = rb(32);
  const priv = crypto.createPrivateKey({ key: Buffer.concat([Buffer.from("302e020100300506032b656e04220420", "hex"), k]), format: "der", type: "pkcs8" });
  const pub = crypto.createPublicKey({ key: Buffer.concat([Buffer.from("302a300506032b656e032100", "hex"), u]), format: "der", type: "spki" });
  let want = null; try { want = crypto.diffieHellman({ privateKey: priv, publicKey: pub }); } catch (e) { want = null; }
  const got = I.x25519Shared(k, u);
  if (want === null) check("random point: OpenSSL refuses, JS must refuse too", got === null);
  else check("X25519 on arbitrary scalar/point differs from OpenSSL", got !== null && Buffer.from(got).equals(want), "k=" + k.toString("hex") + " u=" + u.toString("hex"));
}
for (let n = 0; n < 300 * SCALE; n++) {
  const ikm = rb(1 + ri(80)), salt = rb(ri(70)), info = rb(ri(60)), len = 1 + ri(255);
  const want = Buffer.from(crypto.hkdfSync("sha256", ikm, salt, info, len));
  check("HKDF differs from OpenSSL (len=" + len + ")", Buffer.from(I.hkdfSha256(ikm, salt, info, len)).equals(want));
  const m = rb(ri(300)), key = rb(1 + ri(140));
  check("SHA-256 differs", H(I.sha256(m)) === crypto.createHash("sha256").update(m).digest("hex"));
  check("HMAC differs", H(I.hmacSha256(key, m)) === crypto.createHmac("sha256", key).update(m).digest("hex"));
}
console.log("  A. differential vs OpenSSL: done");

// ---------------------------------------------------------------- B. packet mutation
function pair() {
  const c = I.clientStart(), sv = I.clientStart();
  const shared = I.x25519Shared(c.priv, Buffer.from(sv.pubHex, "hex"));
  const k = I.deriveKeys(shared, c.pubHex, sv.pubHex);
  const sid = H(rb(16));
  return {
    cs: new I.Session(sid, k.c2s, k.s2c, new Uint8Array([0,0,0,1]), new Uint8Array([0,0,0,2]), "c2s", "s2c"),
    mk: () => new I.Session(sid, k.s2c, k.c2s, new Uint8Array([0,0,0,2]), new Uint8Array([0,0,0,1]), "s2c", "c2s"),
    sid
  };
}
{
  const ALPHA = "0123456789abcdefABCDEF._-/%:;|\"'\\{}[] \n\t,=&?#@!$()*+<>^~`vV";
  const { cs, mk, sid } = pair();
  const genuine = cs.seal("a", "the quick brown fox");
  const g = genuine.split(".");
  const equivalent = (m) => {            // same packet spelled differently (hex case, counter leading zeros)
    const q = m.split(".");
    return q.length === 4 && q[0] === "v1" && q[1] === g[1] && /^[0-9]{1,15}$/.test(q[2]) && Number(q[2]) === Number(g[2]) && q[3].toLowerCase() === g[3];
  };
  const srv = mk();
  let tried = 0;
  for (let pos = 0; pos < genuine.length; pos++) {
    for (const ch of ALPHA) {
      const rep = genuine.slice(0, pos) + ch + genuine.slice(pos + 1);
      const ins = genuine.slice(0, pos) + ch + genuine.slice(pos);
      if (rep !== genuine && !equivalent(rep)) { tried++; if (srv.open(rep, "a") !== null) check("replacement accepted at " + pos + " [" + ch + "]", false); }
      if (!equivalent(ins)) { tried++; if (srv.open(ins, "a") !== null) check("insertion accepted at " + pos + " [" + ch + "]", false); }
    }
    const del = genuine.slice(0, pos) + genuine.slice(pos + 1);
    if (!equivalent(del)) { tried++; if (srv.open(del, "a") !== null) check("deletion accepted at " + pos, false); }
    if (srv.open(genuine.slice(0, pos), "a") !== null) check("truncation accepted at " + pos, false);
  }
  check("forged attempts did not break the genuine packet", srv.open(genuine, "a") === "the quick brown fox");
  check("genuine packet cannot be replayed", srv.open(genuine, "a") === null);
  for (let n = 0; n < 3000 * SCALE; n++) {
    const kind = ri(4);
    let p;
    if (kind === 0) p = Buffer.from(rb(ri(200))).toString("latin1");
    else if (kind === 1) p = "v1." + sid + "." + ri(100) + "." + H(rb(16 + ri(60)));
    else if (kind === 2) p = "v1." + H(rb(16)) + "." + ri(10) + "." + H(rb(40));
    else p = ["", ".", "v1", "v1.", "...", "v1...."][ri(6)] + String.fromCharCode(ri(0xd800));
    check("garbage packet opened", mk().open(p, "a") === null, JSON.stringify(p).slice(0, 100));
  }
  console.log("  B. " + tried + " mutated packets + garbage: all rejected");
}

// ---------------------------------------------------------------- C. differential vs the real server
function freePort() { return new Promise((res, rej) => { const sv = net.createServer(); sv.listen(0, "127.0.0.1", () => { const p = sv.address().port; sv.close(() => res(p)); }); sv.on("error", rej); }); }
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
function randomText(maxLen) {
  // valid unicode only (no lone surrogates, no NUL): ASCII, controls, Latin-1, BMP, astral
  const n = ri(maxLen + 1); let out = "";
  for (let i = 0; i < n; i++) {
    const k = ri(10);
    let cp;
    if (k < 4) cp = 32 + ri(95);
    else if (k === 4) cp = 1 + ri(31);                   // control characters (no NUL)
    else if (k === 5) cp = 0x80 + ri(0x780);
    else if (k === 6) cp = 0x800 + ri(0xd000 - 0x800);
    else if (k === 7) cp = 0xe000 + ri(0x1f00);
    else if (k === 8) cp = 0x10000 + ri(0x10000);
    else cp = [34, 92, 47, 8, 12, 10, 13, 9, 127, 0x2028, 0x2029, 0xfeff, 0xffff, 0x7ff, 0x800, 0x10ffff][ri(16)];
    out += String.fromCodePoint(cp);
  }
  return out;
}
(async () => {
  const ROOT = path.join(__dirname, "..", "..");
  const KOSH = process.env.KOSH || path.join(ROOT, process.platform === "win32" ? "kosh.exe" : "kosh");
  const port = await freePort(), base = "http://127.0.0.1:" + port;
  const srv = spawn(KOSH, [path.join(__dirname, "e2e_server.kh"), String(port)], { cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });
  let log = ""; srv.stdout.on("data", d => log += d); srv.stderr.on("data", d => log += d);
  const stop = () => { try { srv.kill("SIGKILL"); } catch (e) {} };
  process.on("exit", stop);
  let up = false;
  for (let i = 0; i < 80 && !up; i++) { try { await fetch(base + "/"); up = true; } catch (e) { await sleep(100); } }
  if (!up) { console.log("server did not start\n" + log); stop(); process.exit(2); }
  try {
    const ch = await Webi.connect(base);
    let bad = 0;
    const N = 300 * SCALE;
    for (let i = 0; i < N; i++) {
      const body = randomText(i % 10 === 0 ? 3000 : 120);
      let r;
      try { r = await ch.request("POST", "/echo", { body, query: "x=" + ri(1000) }); }
      catch (e) { bad++; if (bad <= 5) console.log("  ✗  request threw for body " + JSON.stringify(body).slice(0, 80) + ": " + e.message); continue; }
      let echoed;
      try { echoed = JSON.parse(r.body); } catch (e) { bad++; if (bad <= 5) console.log("  ✗  server returned invalid JSON for body " + JSON.stringify(body).slice(0, 80)); continue; }
      if (echoed.body !== body) { bad++; if (bad <= 5) console.log("  ✗  body not preserved: sent " + JSON.stringify(body).slice(0, 80) + " got " + JSON.stringify(echoed.body).slice(0, 80)); }
    }
    check("C. " + N + " random unicode bodies round-trip exactly through JS client -> Kosh server (" + bad + " bad)", bad === 0);

    // hostile header / path / method strings must give sealed replies, never a crash
    const nasty = ["", "/", "/hello", "/a b", "/%00", "/hello?x=" + "A".repeat(5000), "/" + "a".repeat(3000), "/hello\nX: y", "/é", "/_webi/secure", "/%5fwebi/secure", "//_webi/secure"];
    for (const p of nasty) {
      let r; try { r = await ch.request("GET", p, { headers: { "X-A": randomText(30).replace(/[\r\n]/g, " ") } }); } catch (e) { r = { status: "threw " + e.message }; }
      check("hostile path " + JSON.stringify(p).slice(0, 30) + " gives a sealed reply with a sane status", typeof r.status === "number" && r.status >= 200 && r.status < 500, String(r.status));
    }
    // NUL in a body is a documented text-only limitation: it must not take the server down
    try { await ch.request("POST", "/echo", { body: "a\u0000b" }); } catch (e) {}
    const after = await ch.request("GET", "/hello");
    check("server still healthy after a NUL byte in a body", after.status === 200);
    // many sessions
    for (let i = 0; i < 20; i++) { const c2 = await Webi.connect(base); const r = await c2.request("GET", "/hello"); check("fresh session #" + i + " works", r.status === 200); }
  } catch (e) { check("section C threw: " + (e && e.stack || e), false); }
  stop();
  console.log("  " + pass + "/" + (pass + fail) + " passed   |   " + fail + " failed   (seed " + SEED + ", scale " + SCALE + ")");
  if (fail) console.log("--- server log ---\n" + log.slice(-800));
  process.exit(fail ? 1 : 0);
})();
