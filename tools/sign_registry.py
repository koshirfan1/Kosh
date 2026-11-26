#!/usr/bin/env python3
"""Sign and verify packages/registry.json (Ed25519), so `kh` can trust the registry.

    python3 tools/sign_registry.py keygen  PRIVATE_KEY_FILE      # once; prints the PUBLIC key
    python3 tools/sign_registry.py sign    PRIVATE_KEY_FILE      # writes packages/registry.json.sig
    python3 tools/sign_registry.py verify  [PUBLIC_KEY_HEX]      # checks the .sig (default key: src/kh_pubkey.h)
    python3 tools/sign_registry.py check                         # CI: fails if a key is configured but the .sig is missing/stale

The PRIVATE key file must stay OUTSIDE the repository (keep it offline or in a password manager /
CI secret store). Anyone who has it can publish packages that `kh` will trust. See docs/SIGNING.md.

Needs:  pip install cryptography      (only for keygen/sign; verify/check use the same library)
"""
import os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
REGISTRY = os.path.join(ROOT, "packages", "registry.json")
SIG = REGISTRY + ".sig"
PUBKEY_H = os.path.join(ROOT, "src", "kh_pubkey.h")


def _lib():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
        from cryptography.hazmat.primitives import serialization
        from cryptography.exceptions import InvalidSignature
        return Ed25519PrivateKey, Ed25519PublicKey, serialization, InvalidSignature
    except ImportError:
        sys.exit("This command needs the 'cryptography' package:  pip install cryptography")


def configured_pubkey():
    m = re.search(r'#define\s+KH_REGISTRY_PUBKEY_HEX\s+"([0-9a-fA-F]*)"', open(PUBKEY_H, encoding="utf-8").read())
    if not m:
        sys.exit("could not find KH_REGISTRY_PUBKEY_HEX in src/kh_pubkey.h")
    return m.group(1).lower()


def read_registry():
    # The signature covers the EXACT bytes of the file (no newline normalisation).
    return open(REGISTRY, "rb").read()


def pub_from_hex(h):
    if not re.fullmatch(r"[0-9a-fA-F]{64}", h or ""):
        sys.exit("public key must be exactly 64 hex characters")
    return bytes.fromhex(h)


def verify_bytes(pub_hex, data, sig_hex):
    _, Pub, _, Bad = _lib()
    sig_hex = sig_hex.strip()
    if not re.fullmatch(r"[0-9a-fA-F]{128}", sig_hex):
        return False
    try:
        Pub.from_public_bytes(pub_from_hex(pub_hex)).verify(bytes.fromhex(sig_hex), data)
        return True
    except Bad:
        return False


def cmd_keygen(path):
    Priv, _, ser, _ = _lib()
    if os.path.exists(path):
        sys.exit("refusing to overwrite existing file: " + path)
    if os.path.abspath(path).startswith(ROOT + os.sep):
        sys.exit("refusing to put the PRIVATE key inside the repository (%s). Choose a path outside it." % ROOT)
    k = Priv.generate()
    raw = k.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(raw.hex() + "\n")
    pub = k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw).hex()
    print("private key written to %s (mode 600) - back it up, keep it OFF the repository" % path)
    print("public key (put this in src/kh_pubkey.h as KH_REGISTRY_PUBKEY_HEX, then rebuild kh):")
    print(pub)


def cmd_sign(path):
    Priv, _, ser, _ = _lib()
    try:
        raw = bytes.fromhex(open(path).read().strip())
    except Exception:
        sys.exit("could not read a 64-hex-character private key from " + path)
    if len(raw) != 32:
        sys.exit("private key must be 32 bytes (64 hex characters)")
    k = Priv.from_private_bytes(raw)
    sig = k.sign(read_registry())
    pub = k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw).hex()
    cfg = configured_pubkey()
    if cfg and cfg != pub:
        sys.exit("this private key does not match KH_REGISTRY_PUBKEY_HEX in src/kh_pubkey.h (key %s...): refusing to write a signature kh would reject" % cfg[:12])
    with open(SIG, "w", newline="\n") as f:
        f.write(sig.hex() + "\n")
    print("wrote %s (public key %s...)" % (os.path.relpath(SIG, ROOT), pub[:16]))
    if not cfg:
        print("NOTE: src/kh_pubkey.h has no key yet, so kh is still in unsigned mode. Paste this key there:\n" + pub)


def cmd_verify(pub_hex=None):
    pub_hex = pub_hex or configured_pubkey()
    if not pub_hex:
        sys.exit("no public key given and src/kh_pubkey.h has none")
    if not os.path.exists(SIG):
        sys.exit("FAIL: %s does not exist" % os.path.relpath(SIG, ROOT))
    ok = verify_bytes(pub_hex, read_registry(), open(SIG).read())
    print("registry signature OK" if ok else "FAIL: registry signature does NOT match packages/registry.json")
    sys.exit(0 if ok else 1)


def cmd_check():
    pub = configured_pubkey()
    if not pub:
        print("registry signing: no public key in src/kh_pubkey.h -> kh runs in UNSIGNED mode (see docs/SIGNING.md)")
        return
    if not os.path.exists(SIG):
        sys.exit("FAIL: a public key is configured but packages/registry.json.sig is missing - kh would refuse every install")
    if not verify_bytes(pub, read_registry(), open(SIG).read()):
        sys.exit("FAIL: packages/registry.json changed after it was signed - re-run `sign_registry.py sign <key>` (kh would refuse every install)")
    print("registry signing: signature matches registry.json")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "keygen": cmd_keygen(a[1])
    elif len(a) == 2 and a[0] == "sign": cmd_sign(a[1])
    elif a[:1] == ["verify"] and len(a) <= 2: cmd_verify(a[1] if len(a) == 2 else None)
    elif a == ["check"]: cmd_check()
    else: sys.exit(__doc__)
