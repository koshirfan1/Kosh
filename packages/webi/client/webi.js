/*
 * webi.js - browser/Node client for the Webi encrypted channel (milestone M4).
 *
 * Speaks the protocol in packages/webi/secure.kh + secure_http.kh:
 *   X25519 handshake -> HKDF-SHA256 key schedule -> ChaCha20-Poly1305 packets
 *   with a per-direction counter nonce and a 64-entry replay window.
 *
 * READ docs/webi-threat-model.md FIRST. This layer runs on top of TLS; the
 * handshake is not authenticated by itself. Everything here is plain
 * JavaScript with no dependencies, written to be auditable:
 *
 *   - It is NOT constant-time (JS cannot promise that). The only long-lived
 *     secrets it touches are per-session keys that exist for minutes, in a
 *     process the attacker would already have to control to time it.
 *   - Anything that can run script in the page (XSS, a malicious extension)
 *     can read the session key and every decrypted response. Use a strict CSP.
 *   - Hiding HTML from View-Source is a side effect, not a security property:
 *     the decrypted DOM is visible in DevTools.
 *
 * Usage:
 *   const ch = await Webi.connect();                 // handshake
 *   const r  = await ch.request("GET", "/blog/post", {query: "a=1"});
 *   r.status, r.headers, r.body, r.contentType
 *
 *   Webi.bootstrap();   // for the shell page: load the current URL through
 *                       // the channel and replace the document with it.
 */
(function (root) {
  "use strict";

  var VERSION = 1;
  var WINDOW = 64;
  var MAX_COUNTER = 281474976710655; // 2^48 - 1
  var enc = new TextEncoder();
  var dec = new TextDecoder("utf-8", { fatal: true });

  /* ------------------------------------------------------------- helpers */

  function hexToBytes(hex) {
    if (typeof hex !== "string" || hex.length % 2 !== 0) throw new Error("bad hex");
    var out = new Uint8Array(hex.length / 2);
    for (var i = 0; i < out.length; i++) {
      var b = parseInt(hex.substr(2 * i, 2), 16);
      if (isNaN(b) || !/^[0-9a-fA-F]{2}$/.test(hex.substr(2 * i, 2))) throw new Error("bad hex");
      out[i] = b;
    }
    return out;
  }
  function bytesToHex(b) {
    var s = "";
    for (var i = 0; i < b.length; i++) s += (b[i] < 16 ? "0" : "") + b[i].toString(16);
    return s;
  }
  function concat() {
    var n = 0, i;
    for (i = 0; i < arguments.length; i++) n += arguments[i].length;
    var out = new Uint8Array(n), o = 0;
    for (i = 0; i < arguments.length; i++) { out.set(arguments[i], o); o += arguments[i].length; }
    return out;
  }
  function ctEqual(a, b) {
    if (a.length !== b.length) return false;
    var d = 0;
    for (var i = 0; i < a.length; i++) d |= a[i] ^ b[i];
    return d === 0;
  }
  function randomBytes(n) {
    var c = root.crypto || (typeof require !== "undefined" ? require("crypto").webcrypto : null);
    if (!c || !c.getRandomValues) throw new Error("no secure random source");
    var b = new Uint8Array(n);
    c.getRandomValues(b);
    return b;
  }

  /* ------------------------------------------------------------- SHA-256 */

  var K256 = null, H256 = null;
  function initSha() {
    if (K256) return;
    K256 = new Uint32Array(64); H256 = new Uint32Array(8);
    var composite = {}, count = 0, scale = Math.pow(2, 32);
    for (var c = 2; count < 64; c++) {
      if (!composite[c]) {
        for (var m = c * c; m < 312; m += c) composite[m] = 1;
        if (count < 8) H256[count] = (Math.pow(c, 0.5) * scale) | 0;
        K256[count++] = (Math.pow(c, 1 / 3) * scale) | 0;
      }
    }
  }
  function sha256(data) {
    initSha();
    var len = data.length, bitsHi = Math.floor(len / 0x20000000), bitsLo = (len << 3) >>> 0;
    var padded = Math.ceil((len + 9) / 64) * 64;
    var buf = new Uint8Array(padded);
    buf.set(data); buf[len] = 0x80;
    var dv = new DataView(buf.buffer);
    dv.setUint32(padded - 8, bitsHi); dv.setUint32(padded - 4, bitsLo);
    var h = new Uint32Array(H256), w = new Uint32Array(64), i, j;
    for (var off = 0; off < padded; off += 64) {
      for (i = 0; i < 16; i++) w[i] = dv.getUint32(off + 4 * i);
      for (i = 16; i < 64; i++) {
        var a = w[i - 15], b = w[i - 2];
        var s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
        var s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
      }
      var A = h[0], B = h[1], C = h[2], D = h[3], E = h[4], F = h[5], G = h[6], H = h[7];
      for (i = 0; i < 64; i++) {
        var S1 = ((E >>> 6) | (E << 26)) ^ ((E >>> 11) | (E << 21)) ^ ((E >>> 25) | (E << 7));
        var ch = (E & F) ^ (~E & G);
        var t1 = (H + S1 + ch + K256[i] + w[i]) | 0;
        var S0 = ((A >>> 2) | (A << 30)) ^ ((A >>> 13) | (A << 19)) ^ ((A >>> 22) | (A << 10));
        var maj = (A & B) ^ (A & C) ^ (B & C);
        var t2 = (S0 + maj) | 0;
        H = G; G = F; F = E; E = (D + t1) | 0; D = C; C = B; B = A; A = (t1 + t2) | 0;
      }
      h[0] += A; h[1] += B; h[2] += C; h[3] += D; h[4] += E; h[5] += F; h[6] += G; h[7] += H;
    }
    var out = new Uint8Array(32), odv = new DataView(out.buffer);
    for (j = 0; j < 8; j++) odv.setUint32(4 * j, h[j]);
    return out;
  }
  function hmacSha256(key, msg) {
    if (key.length > 64) key = sha256(key);
    var k = new Uint8Array(64); k.set(key);
    var ipad = new Uint8Array(64), opad = new Uint8Array(64);
    for (var i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    return sha256(concat(opad, sha256(concat(ipad, msg))));
  }
  function hkdfSha256(ikm, salt, info, length) {
    if (!(length >= 1 && length <= 255)) throw new Error("bad hkdf length");
    var prk = hmacSha256(salt.length ? salt : new Uint8Array(32), ikm);
    var okm = new Uint8Array(0), t = new Uint8Array(0);
    for (var i = 1; okm.length < length; i++) {
      t = hmacSha256(prk, concat(t, info, new Uint8Array([i])));
      okm = concat(okm, t);
    }
    return okm.subarray(0, length);
  }

  /* ------------------------------------------------------------ ChaCha20 */

  function rotl(v, n) { return (v << n) | (v >>> (32 - n)); }
  function chachaBlock(key, counter, nonce) {
    var s = new Uint32Array(16), x = new Uint32Array(16), i;
    var kv = new DataView(key.buffer, key.byteOffset, 32), nv = new DataView(nonce.buffer, nonce.byteOffset, 12);
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (i = 0; i < 8; i++) s[4 + i] = kv.getUint32(4 * i, true);
    s[12] = counter >>> 0;
    s[13] = nv.getUint32(0, true); s[14] = nv.getUint32(4, true); s[15] = nv.getUint32(8, true);
    x.set(s);
    function qr(a, b, c, d) {
      x[a] = (x[a] + x[b]) | 0; x[d] = rotl(x[d] ^ x[a], 16);
      x[c] = (x[c] + x[d]) | 0; x[b] = rotl(x[b] ^ x[c], 12);
      x[a] = (x[a] + x[b]) | 0; x[d] = rotl(x[d] ^ x[a], 8);
      x[c] = (x[c] + x[d]) | 0; x[b] = rotl(x[b] ^ x[c], 7);
    }
    for (i = 0; i < 10; i++) {
      qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
      qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
    }
    var out = new Uint8Array(64), ov = new DataView(out.buffer);
    for (i = 0; i < 16; i++) ov.setUint32(4 * i, (x[i] + s[i]) | 0, true);
    return out;
  }
  function chachaXor(key, counter, nonce, data) {
    var out = new Uint8Array(data.length);
    for (var off = 0; off < data.length; off += 64, counter++) {
      var blk = chachaBlock(key, counter, nonce), n = Math.min(64, data.length - off);
      for (var i = 0; i < n; i++) out[off + i] = data[off + i] ^ blk[i];
    }
    return out;
  }

  /* ------------------------------------------------------------ Poly1305 */
  // BigInt implementation: simple and obviously correct (not constant-time).

  function leToBig(b) {
    var v = 0n;
    for (var i = b.length - 1; i >= 0; i--) v = (v << 8n) | BigInt(b[i]);
    return v;
  }
  function poly1305(key, msg) {
    var P = (1n << 130n) - 5n;
    var r = leToBig(key.subarray(0, 16)) & 0x0ffffffc0ffffffc0ffffffc0fffffffn;
    var s = leToBig(key.subarray(16, 32));
    var h = 0n;
    for (var off = 0; off < msg.length; off += 16) {
      var blk = msg.subarray(off, Math.min(off + 16, msg.length));
      var n = leToBig(blk) + (1n << BigInt(8 * blk.length));
      h = ((h + n) * r) % P;
    }
    var tag = (h + s) & ((1n << 128n) - 1n);
    var out = new Uint8Array(16);
    for (var i = 0; i < 16; i++) { out[i] = Number(tag & 0xffn); tag >>= 8n; }
    return out;
  }

  /* ------------------------------------------- ChaCha20-Poly1305 (RFC 8439) */

  function le64(n) {
    var out = new Uint8Array(8);
    for (var i = 0; i < 8; i++) { out[i] = n % 256; n = Math.floor(n / 256); }
    return out;
  }
  function pad16(len) { return new Uint8Array((16 - (len % 16)) % 16); }
  function aeadTag(key, nonce, aad, ct) {
    var otk = chachaBlock(key, 0, nonce).subarray(0, 32);
    return poly1305(otk, concat(aad, pad16(aad.length), ct, pad16(ct.length), le64(aad.length), le64(ct.length)));
  }
  // returns ciphertext || 16-byte tag
  function aeadEncrypt(key, nonce, aad, pt) {
    var ct = chachaXor(key, 1, nonce, pt);
    return concat(ct, aeadTag(key, nonce, aad, ct));
  }
  // returns plaintext bytes, or null if the tag does not verify
  function aeadDecrypt(key, nonce, aad, sealed) {
    if (sealed.length < 16) return null;
    var ct = sealed.subarray(0, sealed.length - 16), tag = sealed.subarray(sealed.length - 16);
    if (!ctEqual(aeadTag(key, nonce, aad, ct), tag)) return null;
    return chachaXor(key, 1, nonce, ct);
  }

  /* -------------------------------------------------------------- X25519 */
  // TweetNaCl-style field arithmetic over GF(2^255-19), 16 limbs of 16 bits
  // in doubles; the ladder uses branch-free conditional swaps.

  function gf(init) { var r = new Float64Array(16); if (init) for (var i = 0; i < init.length; i++) r[i] = init[i]; return r; }
  var _121665 = gf([0xdb41, 1]);

  function car25519(o) {
    var c = 1, v;
    for (var i = 0; i < 16; i++) { v = o[i] + c + 65535; c = Math.floor(v / 65536); o[i] = v - c * 65536; }
    o[0] += c - 1 + 37 * (c - 1);
  }
  function sel25519(p, q, b) {
    var t, c = ~(b - 1);
    for (var i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
  }
  function pack25519(o, n) {
    var i, j, b, m = gf(), t = gf();
    for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (j = 0; j < 2; j++) {
      m[0] = t[0] - 0xffed;
      for (i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
      m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
      b = (m[15] >> 16) & 1;
      m[14] &= 0xffff;
      sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = t[i] >> 8; }
  }
  function unpack25519(o, n) {
    for (var i = 0; i < 16; i++) o[i] = n[2 * i] + (n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
  }
  function A(o, a, b) { for (var i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
  function Z(o, a, b) { for (var i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
  function M(o, a, b) {
    var i, j, t = new Float64Array(31);
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
  }
  function S(o, a) { M(o, a, a); }
  function inv25519(o, i) {
    var c = gf(), a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) { S(c, c); if (a !== 2 && a !== 4) M(c, c, i); }
    for (a = 0; a < 16; a++) o[a] = c[a];
  }
  function x25519(n, p) {
    var z = new Uint8Array(32), i, r;
    var x = new Float64Array(80), a = gf(), b = gf(), c = gf(), d = gf(), e = gf(), f = gf();
    for (i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (i = 254; i >= 0; --i) {
      r = (z[i >>> 3] >>> (i & 7)) & 1;
      sel25519(a, b, r); sel25519(c, d, r);
      A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d);
      S(d, e); S(f, a); M(a, c, a); M(c, b, e);
      A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
      M(a, c, _121665); A(a, a, d); M(c, c, a); M(a, d, f);
      M(d, b, x); S(b, e);
      sel25519(a, b, r); sel25519(c, d, r);
    }
    for (i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    var x32 = x.subarray(32), x16 = x.subarray(16);
    inv25519(x32, x32);
    M(x16, x16, x32);
    var q = new Uint8Array(32);
    pack25519(q, x16);
    return q;
  }
  function x25519Public(priv) { var base = new Uint8Array(32); base[0] = 9; return x25519(priv, base); }
  // null if the result is all zero (low-order peer point)
  function x25519Shared(priv, peerPub) {
    var out = x25519(priv, peerPub), acc = 0;
    for (var i = 0; i < 32; i++) acc |= out[i];
    return acc === 0 ? null : out;
  }

  /* ---------------------------------------------------------- the channel */

  var DIR_C2S = new Uint8Array([0, 0, 0, 1]);
  var DIR_S2C = new Uint8Array([0, 0, 0, 2]);

  function counterBytes(n) {   // 8-byte big-endian
    var out = new Uint8Array(8);
    for (var i = 7; i >= 0; i--) { out[i] = n % 256; n = Math.floor(n / 256); }
    return out;
  }
  function aadFor(sid, dirName, ctr, callerAad) {
    return enc.encode("kosh-webi/" + VERSION + "|" + sid + "|" + dirName + "|" + ctr + "|" + callerAad);
  }
  function isHex(s, n) { return typeof s === "string" && s.length === n && /^[0-9a-f]+$/.test(s); }

  function deriveKeys(shared, clientPubHex, serverPubHex) {
    var saltHex = bytesToHex(sha256(enc.encode("kosh-webi/1" + clientPubHex + serverPubHex)));
    var okm = hkdfSha256(shared, hexToBytes(saltHex), enc.encode("kosh-webi v1 key schedule"), 64);
    return { c2s: okm.subarray(0, 32), s2c: okm.subarray(32, 64) };
  }

  function Session(sid, sendKey, recvKey, sendDir, recvDir, sendName, recvName) {
    this.sid = sid; this.sendKey = sendKey; this.recvKey = recvKey;
    this.sendDir = sendDir; this.recvDir = recvDir;
    this.sendName = sendName; this.recvName = recvName;
    this.sendCtr = 0; this.recvMax = -1; this.recvSeen = new Set();
  }
  // -> "v1.<sid>.<ctr>.<ct_hex>" or null
  Session.prototype.seal = function (callerAad, plaintext) {
    if (this.sendCtr > MAX_COUNTER) return null;
    var ctr = this.sendCtr;
    var nonce = concat(this.sendDir, counterBytes(ctr));
    var ct = aeadEncrypt(this.sendKey, nonce, aadFor(this.sid, this.sendName, ctr, callerAad), enc.encode(plaintext));
    this.sendCtr = ctr + 1;
    return "v1." + this.sid + "." + ctr + "." + bytesToHex(ct);
  };
  // -> plaintext string or null on ANY problem (replay, tamper, wrong session...)
  Session.prototype.open = function (packet, callerAad) {
    if (typeof packet !== "string" || packet.length > 1048576) return null;
    var parts = packet.split(".");
    if (parts.length !== 4 || parts[0] !== "v1" || parts[1] !== this.sid) return null;
    if (!/^[0-9]{1,15}$/.test(parts[2])) return null;
    var ctr = Number(parts[2]);
    if (ctr > MAX_COUNTER) return null;
    if (ctr <= this.recvMax - WINDOW) return null;
    if (ctr <= this.recvMax && this.recvSeen.has(ctr)) return null;
    var sealed;
    try { sealed = hexToBytes(parts[3]); } catch (e) { return null; }
    var nonce = concat(this.recvDir, counterBytes(ctr));
    var pt = aeadDecrypt(this.recvKey, nonce, aadFor(this.sid, this.recvName, ctr, callerAad), sealed);
    if (pt === null) return null;
    // authenticated: only now record the counter
    this.recvSeen.add(ctr);
    if (ctr > this.recvMax) this.recvMax = ctr;
    if (this.recvSeen.size > WINDOW * 2) {
      var floor = this.recvMax - WINDOW + 1, keep = new Set();
      this.recvSeen.forEach(function (c) { if (c >= floor) keep.add(c); });
      this.recvSeen = keep;
    }
    try { return dec.decode(pt); } catch (e) { return null; }
  };

  // Handshake pieces (also used by the tests with fixed keys).
  function clientStart(privBytes) {
    var priv = privBytes || randomBytes(32);
    return { priv: priv, pubHex: bytesToHex(x25519Public(priv)) };
  }
  function clientFinish(client, reply) {
    if (!reply || reply.v !== VERSION || !isHex(reply.sid, 32) || !isHex(reply.pub, 64)) return null;
    var shared = x25519Shared(client.priv, hexToBytes(reply.pub));
    if (shared === null) return null;
    var k = deriveKeys(shared, client.pubHex, reply.pub);
    return new Session(reply.sid, k.c2s, k.s2c, DIR_C2S, DIR_S2C, "c2s", "s2c");
  }

  function headerLines(headers) {
    var s = "";
    if (headers) for (var k in headers) if (Object.prototype.hasOwnProperty.call(headers, k)) s += k + ": " + headers[k] + String.fromCharCode(10);
    return s;
  }

  function Channel(base, session) { this.base = base; this.session = session; }

  // opts: {query: "a=1&b=2", headers: {..}, body: "..."}; a "?query" inside
  // `path` is split off automatically. Resolves to the REAL response:
  // {status, contentType, headers, cookies, body}. Rejects if the channel
  // refuses the packet (replay, expired session, ...) or the reply does not
  // authenticate.
  Channel.prototype.request = async function (method, path, opts) {
    opts = opts || {};
    var query = opts.query || "";
    var q = path.indexOf("?");
    if (q >= 0) {                       // "/p?a=1" + opts.query "b=2" -> "a=1&b=2"
      var inPath = path.slice(q + 1);
      query = inPath && query ? inPath + "&" + query : inPath || query;
      path = path.slice(0, q);
    }
    var plain = JSON.stringify({
      method: method, path: path, query: query,
      headers: headerLines(opts.headers), body: opts.body || ""
    });
    var packet = this.session.seal("webi-req", plain);
    if (packet === null) throw new Error("webi: session exhausted, reconnect");
    var resp = await root.fetch(this.base + "/_webi/secure", {
      method: "POST", body: packet, headers: { "Content-Type": "text/plain" }, cache: "no-store", credentials: "omit"
    });
    var text = await resp.text();
    if (resp.status !== 200) throw new Error("webi: channel refused the request (HTTP " + resp.status + ")");
    var inner = this.session.open(text, "webi-res");
    if (inner === null) throw new Error("webi: response failed authentication");
    var res = JSON.parse(inner);
    return { status: res.status, contentType: res.content_type, headers: res.headers || {}, cookies: res.cookies || [], body: res.body };
  };

  async function connect(base) {
    base = base || "";
    var client = clientStart();
    var resp = await root.fetch(base + "/_webi/handshake", {
      method: "POST", body: JSON.stringify({ v: VERSION, pub: client.pubHex }),
      headers: { "Content-Type": "application/json" }, cache: "no-store", credentials: "omit"
    });
    if (resp.status !== 200) throw new Error("webi: handshake refused (HTTP " + resp.status + ")");
    var session = clientFinish(client, await resp.json());
    if (session === null) throw new Error("webi: bad handshake reply");
    return new Channel(base, session);
  }

  // For the shell page served to browsers: fetch the page the user asked for
  // through the channel and replace the document with it.
  async function bootstrap(base) {
    var loc = root.location, ch = await connect(base || "");
    var r = await ch.request("GET", loc.pathname, { query: loc.search ? loc.search.slice(1) : "" });
    var d = root.document;
    if (/html/i.test(r.contentType || "")) { d.open(); d.write(r.body); d.close(); }
    else { d.body.textContent = r.body; }
    return r;
  }

  var Webi = {
    version: VERSION, connect: connect, bootstrap: bootstrap, Channel: Channel,
    _internal: {
      sha256: sha256, hmacSha256: hmacSha256, hkdfSha256: hkdfSha256,
      chachaBlock: chachaBlock, poly1305: poly1305,
      aeadEncrypt: aeadEncrypt, aeadDecrypt: aeadDecrypt,
      x25519: x25519, x25519Public: x25519Public, x25519Shared: x25519Shared,
      clientStart: clientStart, clientFinish: clientFinish, deriveKeys: deriveKeys,
      hexToBytes: hexToBytes, bytesToHex: bytesToHex, Session: Session
    }
  };
  if (typeof module !== "undefined" && module.exports) module.exports = Webi;
  root.Webi = Webi;

  // <script src="/_webi/client.js" data-webi-bootstrap></script> is all the
  // shell page needs (no inline script, so a strict CSP can stay on).
  var cs = root.document && root.document.currentScript;
  if (cs && cs.hasAttribute && cs.hasAttribute("data-webi-bootstrap")) {
    bootstrap().catch(function (e) {
      var b = root.document.body || root.document.documentElement;
      b.textContent = "Secure channel failed: " + (e && e.message ? e.message : e);
    });
  }
})(typeof globalThis !== "undefined" ? globalThis : this);
