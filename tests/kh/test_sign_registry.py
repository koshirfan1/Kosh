#!/usr/bin/env python3
"""Tests for tools/sign_registry.py, run in a scratch copy of the repo layout so the real
src/kh_pubkey.h and packages/registry.json.sig are never touched.

    python3 tests/kh/test_sign_registry.py
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
passed = failed = 0

def check(name, cond, detail=""):
    global passed, failed
    if cond: passed += 1
    else:
        failed += 1
        print("  \u2717  FAIL  " + name + ("\n       " + detail if detail else ""))

try:
    import cryptography  # noqa: F401
except ImportError:
    print("SKIP: python 'cryptography' not available"); sys.exit(0)

tmp = tempfile.mkdtemp(prefix="sign_test_")
repo = os.path.join(tmp, "repo")
for d in ("tools", "src", "packages"):
    os.makedirs(os.path.join(repo, d))
shutil.copy(os.path.join(ROOT, "tools", "sign_registry.py"), os.path.join(repo, "tools"))
shutil.copy(os.path.join(ROOT, "packages", "registry.json"), os.path.join(repo, "packages"))
header = os.path.join(repo, "src", "kh_pubkey.h")
shutil.copy(os.path.join(ROOT, "src", "kh_pubkey.h"), header)
tool = os.path.join(repo, "tools", "sign_registry.py")
keyfile = os.path.join(tmp, "private.key")           # outside the repo copy, as required
sig = os.path.join(repo, "packages", "registry.json.sig")

def run(*args):
    r = subprocess.run([sys.executable, tool] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr

def set_pubkey(hexkey):
    s = open(header).read()
    s = re.sub(r'(#define\s+KH_REGISTRY_PUBKEY_HEX\s+)"[0-9a-fA-F]*"', r'\1"%s"' % hexkey, s)
    open(header, "w").write(s)

try:
    rc, out = run("check")
    check("shipped state: no key configured -> check passes and says UNSIGNED", rc == 0 and "UNSIGNED" in out, out)

    rc, out = run("keygen", os.path.join(repo, "inside.key"))
    check("keygen refuses a private-key path inside the repository", rc != 0 and not os.path.exists(os.path.join(repo, "inside.key")), out)
    rc, out = run("keygen", keyfile)
    pub = out.strip().splitlines()[-1]
    check("keygen writes a key and prints a 64-hex public key", rc == 0 and re.fullmatch(r"[0-9a-f]{64}", pub) is not None, out)
    if os.name != "nt":
        check("private key file is mode 600", (os.stat(keyfile).st_mode & 0o777) == 0o600)
    rc, out = run("keygen", keyfile)
    check("keygen refuses to overwrite an existing key", rc != 0, out)

    rc, out = run("sign", keyfile)
    check("sign works while no key is configured, and tells you what to paste", rc == 0 and pub in out and os.path.exists(sig), out)
    rc, out = run("verify", pub)
    check("verify with the explicit public key passes", rc == 0 and "OK" in out, out)

    set_pubkey(pub)
    rc, out = run("check")
    check("key configured + fresh signature: check passes", rc == 0 and "matches" in out, out)
    rc, out = run("verify")
    check("verify uses the key from kh_pubkey.h", rc == 0, out)

    # registry edited after signing
    with open(os.path.join(repo, "packages", "registry.json"), "ab") as f: f.write(b" ")
    rc, out = run("check")
    check("registry edited after signing: check FAILS (this is the CI guard)", rc != 0 and "re-run" in out, out)
    rc, out = run("verify")
    check("registry edited after signing: verify fails", rc != 0, out)
    rc, out = run("sign", keyfile)
    rc2, out2 = run("check")
    check("re-signing fixes it", rc == 0 and rc2 == 0, out + out2)

    os.remove(sig)
    rc, out = run("check")
    check("key configured but .sig missing: check FAILS", rc != 0 and "missing" in out, out)
    run("sign", keyfile)

    # wrong private key must not produce a signature kh would reject
    other = os.path.join(tmp, "other.key")
    run("keygen", other)
    before = open(sig).read()
    rc, out = run("sign", other)
    check("sign refuses a private key that does not match kh_pubkey.h", rc != 0 and open(sig).read() == before, out)

    # garbage in the .sig
    for label, text in (("short", "abcd"), ("non-hex", "z" * 128), ("empty", "")):
        open(sig, "w").write(text)
        rc, out = run("check")
        check("malformed .sig (%s): check FAILS" % label, rc != 0, out)
    rc, out = run("verify", "nothex")
    check("verify rejects a malformed public key argument", rc != 0, out)
finally:
    shutil.rmtree(tmp, ignore_errors=True)

print("  %d/%d passed   |   %d failed" % (passed, passed + failed, failed))
sys.exit(1 if failed else 0)
