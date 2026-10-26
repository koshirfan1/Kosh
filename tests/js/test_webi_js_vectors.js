// Vector and cross-implementation tests for packages/webi/client/webi.js.
// Ground truth: RFC test vectors + Node's built-in OpenSSL (node:crypto).
"use strict";
const crypto = require("crypto");
const Webi = require("../../packages/webi/client/webi.js");
const I = Webi._internal;
const hex = (h) => I.hexToBytes(h);
const H = (b) => I.bytesToHex(b);

let pass = 0, fail = 0;
function eq(name, got, want) {
  if (got === want) { pass++; } else { fail++; console.log("  ✗  FAIL  " + name + "\n       got  " + got + "\n       want " + want); }
}
function ok(name, cond) { eq(name, !!cond, true); }

// ---- SHA-256 / HMAC / HKDF ---------------------------------------------------
eq("sha256('')", H(I.sha256(new Uint8Array(0))), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
eq("sha256('abc')", H(I.sha256(Buffer.from("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
eq("sha256 448-bit message (FIPS 180-4)", H(I.sha256(Buffer.from("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
eq("HMAC RFC 4231 case 2", H(I.hmacSha256(Buffer.from("Jefe"), Buffer.from("what do ya want for nothing?"))), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
eq("HMAC with 131-byte key (RFC 4231 case 6)", H(I.hmacSha256(Buffer.alloc(131, 0xaa), Buffer.from("Test Using Larger Than Block-Size Key - Hash Key First"))), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
eq("HKDF RFC 5869 case 1", H(I.hkdfSha256(Buffer.alloc(22, 0x0b), hex("000102030405060708090a0b0c"), hex("f0f1f2f3f4f5f6f7f8f9"), 42)), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
eq("HKDF RFC 5869 case 3 (empty salt/info)", H(I.hkdfSha256(Buffer.alloc(22, 0x0b), new Uint8Array(0), new Uint8Array(0), 42)), "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");
for (let n of [0, 1, 55, 56, 63, 64, 65, 119, 120, 1000]) {
  const m = crypto.randomBytes(n);
  eq("sha256 matches OpenSSL, " + n + " bytes", H(I.sha256(m)), crypto.createHash("sha256").update(m).digest("hex"));
}

// ---- ChaCha20-Poly1305: the official RFC 8439 section 2.8.2 vector --------------------
{
  const key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
  const nonce = hex("070000004041424344454647");
  const aad = hex("50515253c0c1c2c3c4c5c6c7");
  const pt = Buffer.from("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
  const out = I.aeadEncrypt(key, nonce, aad, pt);
  eq("RFC 8439 2.8.2 tag", H(out.subarray(out.length - 16)), "1ae10b594f09e26a7e902ecbd0600691");
  eq("RFC 8439 2.8.2 ciphertext head", H(out.subarray(0, 16)), "d31a8d34648e60db7b86afbc53ef7ec2");
  eq("RFC 8439 2.8.2 decrypts", Buffer.from(I.aeadDecrypt(key, nonce, aad, out)).toString(), pt.toString());
}
// RFC 8439 2.5.2 Poly1305 vector
eq("RFC 8439 2.5.2 Poly1305", H(I.poly1305(hex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"), Buffer.from("Cryptographic Forum Research Group"))), "a8061dc1305136c6c22b8baf0c0127a9");

// ---- ChaCha20-Poly1305 vs OpenSSL on many sizes -------------------------------------------
for (const n of [0, 1, 15, 16, 17, 31, 32, 63, 64, 65, 127, 128, 129, 1000, 4096]) {
  for (const an of [0, 5, 16, 33]) {
    const key = crypto.randomBytes(32), nonce = crypto.randomBytes(12), aad = crypto.randomBytes(an), pt = crypto.randomBytes(n);
    const c = crypto.createCipheriv("chacha20-poly1305", key, nonce, { authTagLength: 16 });
    c.setAAD(aad, { plaintextLength: n });
    const ct = Buffer.concat([c.update(pt), c.final()]);
    const want = Buffer.concat([ct, c.getAuthTag()]).toString("hex");
    eq("encrypt matches OpenSSL, pt=" + n + " aad=" + an, H(I.aeadEncrypt(key, nonce, aad, pt)), want);
    const back = I.aeadDecrypt(key, nonce, aad, hex(want));
    ok("decrypts OpenSSL output, pt=" + n + " aad=" + an, back && Buffer.from(back).equals(pt));
  }
}
{
  const key = crypto.randomBytes(32), nonce = crypto.randomBytes(12), pt = Buffer.from("tamper test");
  const sealed = I.aeadEncrypt(key, nonce, Buffer.from("a"), pt);
  const flip = (i) => { const c = Uint8Array.from(sealed); c[i] ^= 1; return c; };
  ok("flipped ciphertext bit rejected", I.aeadDecrypt(key, nonce, Buffer.from("a"), flip(0)) === null);
  ok("flipped tag bit rejected", I.aeadDecrypt(key, nonce, Buffer.from("a"), flip(sealed.length - 1)) === null);
  ok("wrong aad rejected", I.aeadDecrypt(key, nonce, Buffer.from("b"), sealed) === null);
  ok("wrong key rejected", I.aeadDecrypt(crypto.randomBytes(32), nonce, Buffer.from("a"), sealed) === null);
  ok("too short rejected", I.aeadDecrypt(key, nonce, Buffer.from("a"), new Uint8Array(5)) === null);
}

// ---- X25519 -------------------------------------------------------------------------------------
{
  const ap = hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const bp = hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  eq("RFC 7748 Alice public", H(I.x25519Public(ap)), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  eq("RFC 7748 Bob public", H(I.x25519Public(bp)), "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  eq("RFC 7748 shared (A)", H(I.x25519Shared(ap, I.x25519Public(bp))), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  eq("RFC 7748 shared (B)", H(I.x25519Shared(bp, I.x25519Public(ap))), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  // RFC 7748 section 5.2 first test vector
  eq("RFC 7748 5.2 vector 1", H(I.x25519(hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"), hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"))), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
  // RFC 7748 section 5.2 iterated: 1 and 1000 iterations
  let k = new Uint8Array(32); k[0] = 9; let u = Uint8Array.from(k);
  const step = () => { const out = I.x25519(k, u); u = k; k = out; };
  step();
  eq("RFC 7748 iterated x1", H(k), "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079");
  for (let i = 1; i < 1000; i++) step();
  eq("RFC 7748 iterated x1000", H(k), "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
  ok("all-zero (low-order) peer is refused", I.x25519Shared(ap, new Uint8Array(32)) === null);
  const one = new Uint8Array(32); one[0] = 1;
  ok("low-order point 1 is refused", I.x25519Shared(ap, one) === null);
  for (let i = 0; i < 20; i++) {
    const a = crypto.generateKeyPairSync("x25519"), b = crypto.generateKeyPairSync("x25519");
    const aPriv = a.privateKey.export({ type: "pkcs8", format: "der" }).subarray(-32);
    const bPubRaw = b.publicKey.export({ type: "spki", format: "der" }).subarray(-32);
    const aPubRaw = a.publicKey.export({ type: "spki", format: "der" }).subarray(-32);
    eq("x25519 public matches OpenSSL #" + i, H(I.x25519Public(aPriv)), aPubRaw.toString("hex"));
    const want = crypto.diffieHellman({ privateKey: a.privateKey, publicKey: b.publicKey }).toString("hex");
    eq("x25519 shared matches OpenSSL #" + i, H(I.x25519Shared(aPriv, bPubRaw)), want);
  }
}

// ---- session: counters, replay window, direction ----------------------------------------------
{
  const c = I.clientStart();
  // build a matching server-side session by hand from a second keypair
  const s = I.clientStart();
  const shared = I.x25519Shared(c.priv, hex(s.pubHex));
  const k = I.deriveKeys(shared, c.pubHex, s.pubHex);
  const sid = "00112233445566778899aabbccddeeff";
  const cs = new I.Session(sid, k.c2s, k.s2c, new Uint8Array([0,0,0,1]), new Uint8Array([0,0,0,2]), "c2s", "s2c");
  const ss = new I.Session(sid, k.s2c, k.c2s, new Uint8Array([0,0,0,2]), new Uint8Array([0,0,0,1]), "s2c", "c2s");
  const p0 = cs.seal("a", "hello");
  eq("server opens client packet", ss.open(p0, "a"), "hello");
  ok("replay rejected", ss.open(p0, "a") === null);
  ok("wrong caller aad rejected", ss.open(cs.seal("a", "x"), "b") === null);
  const pk = []; for (let i = 0; i < 5; i++) pk.push(cs.seal("w", "m" + i));
  eq("out of order: newest first", ss.open(pk[4], "w"), "m4");
  eq("older packet in window", ss.open(pk[2], "w"), "m2");
  ok("but only once", ss.open(pk[2], "w") === null);
  const first = cs.seal("o", "first"); let last;
  for (let i = 0; i < 70; i++) last = cs.seal("o", "filler");
  eq("jump ahead accepted", ss.open(last, "o"), "filler");
  ok("older than the window rejected", ss.open(first, "o") === null);
  const back = ss.seal("r", "server says");
  ok("a packet cannot be reflected back to its sender's direction", cs.open(cs.seal("r", "x"), "r") === null);
  eq("client opens server packet", cs.open(back, "r"), "server says");
  ok("garbage rejected", ss.open("not a packet", "a") === null);
  ok("wrong sid rejected", ss.open(p0.replace(sid, "ffffffffffffffffffffffffffffffff"), "a") === null);
  ok("non-hex ciphertext rejected", ss.open("v1." + sid + ".99.zz", "a") === null);
  cs.sendCtr = 281474976710656;
  ok("sealing past 2^48 refuses", cs.seal("a", "x") === null);
}

console.log("  " + pass + "/" + (pass + fail) + " passed   |   " + fail + " failed");
process.exit(fail ? 1 : 0);
