#!/usr/bin/env python3
"""Tests for src/kh_ed25519.h (the signature verifier inside kh).

Compiles tests/kh/test_ed25519.c (with ASan/UBSan when gcc supports it) and checks it against
  * the RFC 8032 section 7.1 test vectors,
  * hashlib's SHA-512 for lengths around every block boundary,
  * signatures made by an independent implementation (python `cryptography`): valid ones must
    verify; any change to the message, signature or key, a wrong key, a non-canonical S (S+L)
    and garbage public keys must NOT verify.

    python3 tests/kh/test_ed25519.py
Skips (exit 0, with a message) if `cryptography` or a C compiler is missing.
"""
import hashlib, os, random, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
passed = failed = 0

def check(name, cond, detail=""):
    global passed, failed
    if cond: passed += 1
    else:
        failed += 1
        print("  \u2717  FAIL  " + name + ("\n       " + detail if detail else ""))

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
except Exception as e:
    print("SKIP: python 'cryptography' not available (%s)" % e); sys.exit(0)

tmp = tempfile.mkdtemp(prefix="ed_test_")
exe = os.path.join(tmp, "test_ed")
src = os.path.join(ROOT, "tests", "kh", "test_ed25519.c")
for flags in (["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], []):
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror"] + flags + ["-o", exe, src], capture_output=True, text=True)
    if r.returncode == 0: break
if r.returncode != 0:
    print("SKIP: could not compile (%s)" % r.stderr[:200]); sys.exit(0)

def run(lines):
    p = subprocess.run([exe], input="\n".join(lines) + "\n", capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if p.returncode != 0:
        print("driver failed:", p.stderr[:300]); sys.exit(1)
    return p.stdout.split()

def h(b): return b.hex() if b else "-"
def verify_many(cases):  # cases: (pk, sig, msg) bytes
    return run(["verify %s %s %s" % (h(pk), h(sig), h(m)) for pk, sig, m in cases])

# ---- SHA-512 ------------------------------------------------------------------
lens = [0, 1, 55, 56, 111, 112, 113, 127, 128, 129, 239, 240, 255, 256, 257, 1000, 4096]
rnd = random.Random(1234)
msgs = [bytes(rnd.getrandbits(8) for _ in range(n)) for n in lens]
got = run(["sha512 " + h(m) for m in msgs])
for n, m, g in zip(lens, msgs, got):
    check("sha512 matches hashlib at length %d" % n, g == hashlib.sha512(m).hexdigest(), g)
check("sha512('abc') known answer", run(["sha512 616263"])[0] == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")

# ---- RFC 8032 section 7.1 -------------------------------------------------------
# The RFC lists SECRET keys; Ed25519 is deterministic, so signing with an independent
# implementation must reproduce the published signature byte for byte.
RFC_SECRET = [
 ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "", "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
 ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "72", "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"),
 ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "af82", "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a", "fc51cd8e6218a0a0ad17d2c4d9e7b3a8fcf7f4d36d2e3d6c4d1b7c3f4c8e2e55"),
]
for sk_hex, m_hex, sig_hex, pk_hex in RFC_SECRET:
    sk = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(sk_hex[:64]))
    pk = sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    m = bytes.fromhex(m_hex)
    sig = sk.sign(m)
    check("RFC 8032 vector: signature is the published one", sig.hex() == sig_hex, sig.hex())
    check("RFC 8032 vector verifies in C", verify_many([(pk, sig, m)]) == ["1"])

# ---- cross-check against `cryptography` -------------------------------------------
L = 2**252 + 27742317777372353535851937790883648493
cases, expect, labels = [], [], []
for i in range(40):
    sk = Ed25519PrivateKey.generate()
    pk = sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    n = rnd.choice([0, 1, 2, 31, 32, 33, 63, 64, 65, 100, 500, 2000, 40000])
    m = bytes(rnd.getrandbits(8) for _ in range(n))
    sig = sk.sign(m)
    cases.append((pk, sig, m)); expect.append("1"); labels.append("valid #%d (len %d)" % (i, n))
    # tampered message
    if n:
        m2 = bytearray(m); m2[rnd.randrange(n)] ^= 1 << rnd.randrange(8)
        cases.append((pk, sig, bytes(m2))); expect.append("0"); labels.append("message bit flipped #%d" % i)
    cases.append((pk, sig, m + b"x")); expect.append("0"); labels.append("message extended #%d" % i)
    # tampered signature (R part and S part)
    for where in (rnd.randrange(32), 32 + rnd.randrange(32)):
        s2 = bytearray(sig); s2[where] ^= 1 << rnd.randrange(8)
        cases.append((pk, bytes(s2), m)); expect.append("0"); labels.append("signature byte %d flipped #%d" % (where, i))
    # wrong key
    other = Ed25519PrivateKey.generate().public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    cases.append((other, sig, m)); expect.append("0"); labels.append("wrong key #%d" % i)
    # non-canonical S: S + L encodes the same scalar mod L but must be rejected
    s_int = int.from_bytes(sig[32:], "little")
    if s_int + L < 2**256:
        mall = sig[:32] + (s_int + L).to_bytes(32, "little")
        cases.append((pk, mall, m)); expect.append("0"); labels.append("non-canonical S (S+L) #%d" % i)
    # truncated / zero signature
    cases.append((pk, b"\x00" * 64, m)); expect.append("0"); labels.append("all-zero signature #%d" % i)
res = verify_many(cases)
bad = [labels[i] for i in range(len(cases)) if res[i] != expect[i]]
check("%d cross-check cases (valid verify, every tamper/wrong-key/non-canonical case is rejected)" % len(cases), not bad, "; ".join(bad[:5]))

# garbage public keys (not on the curve / small order) must never verify
sk = Ed25519PrivateKey.generate(); m = b"registry"; sig = sk.sign(m)
junk = [bytes([0xff] * 32), bytes(32), bytes([1] + [0] * 31), bytes([2] + [0] * 31)] + [bytes(rnd.getrandbits(8) for _ in range(32)) for _ in range(30)]
res = verify_many([(k, sig, m) for k in junk])
check("garbage / small-order public keys never verify this signature", set(res) == {"0"}, str(res))

print("  %d/%d passed   |   %d failed" % (passed, passed + failed, failed))
sys.exit(1 if failed else 0)
