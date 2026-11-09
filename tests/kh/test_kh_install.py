#!/usr/bin/env python3
"""Tests for `kh install`: SHA-256 verification, staged/atomic installs, dependencies,
fail-closed behaviour and input validation. Runs against a local file:// registry built
from the real packages/ folder, with HOME pointed at a scratch directory, so it needs
no network.

    python3 tests/kh/test_kh_install.py          # uses ./kh and ./kosh (run `make` first)
    KH=/path/to/kh KOSH=/path/to/kosh python3 tests/kh/test_kh_install.py
"""
import hashlib, json, os, random, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXE = ".exe" if os.name == "nt" else ""
KH = os.environ.get("KH", os.path.join(ROOT, "kh" + EXE))
KOSH = os.environ.get("KOSH", os.path.join(ROOT, "kosh" + EXE))
SRC_PACKAGES = os.path.join(ROOT, "packages")

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  \u2717  FAIL  " + name + ("\n       " + detail if detail else ""))


class Env:
    """A scratch world: a web root (copy of packages/ with file:// URLs) and an empty HOME."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="kh_test_")
        self.web = os.path.join(self.tmp, "web", "packages")
        self.home = os.path.join(self.tmp, "home")
        os.makedirs(self.home)
        shutil.copytree(SRC_PACKAGES, self.web)
        self.reg_path = os.path.join(self.web, "registry.json")
        self.reg = json.load(open(self.reg_path, encoding="utf-8"))
        for p in self.reg["packages"]:
            p["url"] = "file://%s/%s/%s.kh" % (self.web, p["name"], p["name"])
        self.save()

    def save(self):
        json.dump(self.reg, open(self.reg_path, "w", encoding="utf-8"), indent=2)

    def entry(self, name):
        return next(p for p in self.reg["packages"] if p["name"] == name)

    def add_fake(self, name, deps=None):
        d = os.path.join(self.web, name)
        os.makedirs(d, exist_ok=True)
        body = ("# fake package %s\n" % name).encode()
        open(os.path.join(d, name + ".kh"), "wb").write(body)
        e = {"name": name, "version": "1.0.0", "description": "fake", "author": "t",
             "url": "file://%s/%s/%s.kh" % (self.web, name, name),
             "sha256": {name + ".kh": hashlib.sha256(body).hexdigest()}}
        if deps:
            e["dependencies"] = deps
        self.reg["packages"].append(e)

    def pkg_file(self, name, rel=None):
        return os.path.join(self.web, name, rel or (name + ".kh"))

    def installed(self, name):
        return os.path.join(self.home, ".kosh", "packages", name)

    def is_installed(self, name):
        return os.path.exists(os.path.join(self.installed(name), name + ".kh"))

    def kh(self, *args, env=None):
        e = dict(os.environ)
        e.update({"HOME": self.home, "USERPROFILE": self.home,
                  "KH_REGISTRY_URL": "file://" + self.reg_path, "KH_RAW_BASE": "file://" + self.web})
        e.update(env or {})
        r = subprocess.run([KH] + list(args), capture_output=True, text=True, env=e, timeout=120)
        return r.returncode, r.stdout + r.stderr

    def run_kosh(self, code):
        cwd = os.path.join(self.tmp, "cwd")
        os.makedirs(cwd, exist_ok=True)
        script = os.path.join(cwd, "t.kh")
        open(script, "w").write(code)
        env = dict(os.environ, HOME=self.home, USERPROFILE=self.home)
        r = subprocess.run([KOSH, script], capture_output=True, text=True, env=env, cwd=cwd, timeout=60)
        return r.returncode, r.stdout + r.stderr

    def leftovers(self):
        base = os.path.join(self.home, ".kosh", "packages")
        return [d for d in os.listdir(base) if d.startswith(".kh-staging")] if os.path.isdir(base) else []

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


def tamper(path):
    with open(path, "ab") as f:
        f.write(b"\n# tampered\n")


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


# --------------------------------------------------------------------------- sha256 command
def test_sha256_command():
    e = Env()
    rnd = random.Random(7)
    sizes = [0, 1, 3, 54, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 1000, 65535, 65536, 65537, 200000]
    bad = []
    for n in sizes:
        p = os.path.join(e.tmp, "f%d.bin" % n)
        open(p, "wb").write(bytes(rnd.getrandbits(8) for _ in range(n)))
        rc, out = e.kh("sha256", p)
        if rc != 0 or out.split()[0] != sha(p):
            bad.append(n)
    check("sha256 matches hashlib for every padding boundary and multi-block size", not bad, "wrong for sizes %s" % bad)
    p = os.path.join(e.tmp, "abc.txt"); open(p, "wb").write(b"abc")
    check("NIST vector: sha256('abc')", e.kh("sha256", p)[1].split()[0] == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    p2 = os.path.join(e.tmp, "nist2.txt"); open(p2, "wb").write(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
    check("NIST vector: 448-bit message", e.kh("sha256", p2)[1].split()[0] == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    check("sha256 of a missing file fails", e.kh("sha256", os.path.join(e.tmp, "does-not-exist"))[0] != 0)
    e.cleanup()


# --------------------------------------------------------------------------- good installs
def test_good_installs():
    e = Env()
    rc, out = e.kh("install", "math")
    check("single-file package installs", rc == 0, out)
    check("it reports the checksum verification", "Verified 1 file" in out, out)
    check("file content is exactly the source", e.is_installed("math") and
          sha(os.path.join(e.installed("math"), "math.kh")) == sha(e.pkg_file("math")))
    check("no staging directory is left behind", e.leftovers() == [])

    for name, expr in [("webi", "print webi_version()"), ("vision", 'print "vision ok"'), ("tensor", 'print "tensor ok"')]:
        rc, out = e.kh("install", name)
        check("%s installs (%d files)" % (name, 1 + len(e.entry(name).get("files", []))), rc == 0, out)
        rc, out = e.run_kosh('import "%s"\n%s\n' % (name, expr))
        check("%s can be imported after install (no missing sibling files)" % name, rc == 0 and "rror" not in out, out)
    check("every shipped webi file is present", all(os.path.exists(os.path.join(e.installed("webi"), f)) for f in e.entry("webi")["files"]))
    check("webi's secure channel files are shipped (regression: registry used to omit them)", all(
        f in e.entry("webi")["files"] for f in ["secure.kh", "secure_http.kh", "bot.kh", "client_js.kh"]))
    e.cleanup()


# --------------------------------------------------------------------------- dependencies
def test_dependencies():
    e = Env()
    rc, out = e.kh("install", "openai")
    check("openai installs", rc == 0, out)
    check("its dependency (requests) was installed first", e.is_installed("requests") and "needs requests" in out, out)
    rc, out = e.run_kosh('import "openai"\nprint "ok"\n')
    check("openai imports after a single `kh install openai` (regression: it used to fail on requests)", rc == 0 and "ok" in out, out)
    e.cleanup()

    e = Env()
    rc, out = e.kh("install", "nn")
    check("nn pulls in tensor", rc == 0 and e.is_installed("tensor"), out)
    rc, out = e.run_kosh('import "nn"\nprint "ok"\n')
    check("nn imports after install", rc == 0 and "ok" in out, out)
    for pkg in ("swagger", "webi_auth", "webi_socket"):
        rc, out = e.kh("install", pkg)
        check("%s installs and pulls in webi" % pkg, rc == 0 and e.is_installed("webi"), out)
    rc, out = e.kh("install", "orm")
    check("orm pulls in json_db", rc == 0 and e.is_installed("json_db"), out)
    e.cleanup()

    e = Env()
    e.kh("install", "requests")
    rc, out = e.kh("install", "openai")
    check("an already-installed dependency is not fetched again", rc == 0 and "installing it first" not in out, out)
    e.cleanup()

    e = Env()
    tamper(e.pkg_file("requests"))
    rc, out = e.kh("install", "openai")
    check("a tampered dependency fails the whole install", rc != 0 and "Could not install dependency 'requests'" in out, out)
    check("...the dependent is not installed", not e.is_installed("openai"))
    check("...and neither is the bad dependency", not e.is_installed("requests"))
    check("...and no staging leftovers", e.leftovers() == [])
    rc, out = e.kh("install", "openai", "--no-verify")
    check("--no-verify applies to dependencies too", rc == 0 and e.is_installed("requests") and e.is_installed("openai"), out)
    e.cleanup()

    e = Env()
    e.entry("openai")["dependencies"] = ["no_such_dependency_xyz"]
    e.save()
    rc, out = e.kh("install", "openai")
    check("a dependency missing from the registry fails clearly", rc != 0 and "not found" in out and "Could not install dependency" in out, out)
    check("...and the dependent is not installed", not e.is_installed("openai"))
    e.entry("openai")["dependencies"] = ["../evil"]
    e.save()
    rc, out = e.kh("install", "openai")
    check("an invalid dependency name is refused", rc != 0 and "invalid dependency name" in out, out)
    e.cleanup()

    e = Env()
    e.entry("math")["dependencies"] = ["strings"]
    e.entry("strings")["dependencies"] = ["math"]
    e.save()
    rc, out = e.kh("install", "math")
    check("a dependency cycle does not recurse forever", rc == 0, out)
    check("...and both packages end up installed", e.is_installed("math") and e.is_installed("strings"))
    e.cleanup()

    e = Env()
    for i in range(10):
        e.add_fake("chain%d" % i, ["chain%d" % (i + 1)] if i < 9 else None)
    e.save()
    rc, out = e.kh("install", "chain0")
    check("a 10-deep dependency chain is refused", rc != 0 and "too deep" in out, out)
    check("...installing nothing from it", not e.is_installed("chain0"))
    rc, out = e.kh("install", "chain6")
    check("a 4-deep chain installs", rc == 0 and e.is_installed("chain9") and e.is_installed("chain6"), out)
    e.cleanup()


# --------------------------------------------------------------------------- tampering
def test_tampering():
    e = Env()
    tamper(e.pkg_file("math"))
    rc, out = e.kh("install", "math")
    check("tampered main file is rejected", rc != 0 and "Checksum mismatch" in out, out)
    check("it shows expected and actual hashes", "expected" in out and "got" in out, out)
    check("nothing was installed", not os.path.exists(e.installed("math")), out)
    check("staging directory was cleaned up", e.leftovers() == [])
    e.cleanup()

    e = Env()
    tamper(e.pkg_file("webi", "util.kh"))
    rc, out = e.kh("install", "webi")
    check("tampered EXTRA file is rejected", rc != 0 and "util.kh" in out and "Checksum mismatch" in out, out)
    check("a half-verified package is never installed (atomic)", not os.path.exists(e.installed("webi")), out)
    check("no staging leftovers after a multi-file failure", e.leftovers() == [])
    e.cleanup()

    e = Env()
    open(e.pkg_file("math"), "wb").write(open(e.pkg_file("math"), "rb").read()[:50])
    rc, out = e.kh("install", "math")
    check("truncated download is rejected", rc != 0 and "Checksum mismatch" in out, out)
    e.cleanup()

    e = Env()
    e.entry("math")["sha256"]["math.kh"] = sha(e.pkg_file("strings"))
    e.save()
    rc, out = e.kh("install", "math")
    check("a checksum belonging to a different file is rejected", rc != 0 and "Checksum mismatch" in out, out)
    e.cleanup()

    e = Env()
    e.kh("install", "math")
    good = open(os.path.join(e.installed("math"), "math.kh"), "rb").read()
    tamper(e.pkg_file("math"))
    rc, out = e.kh("install", "math")
    check("a failed upgrade is rejected", rc != 0, out)
    check("...and the existing good install is left exactly as it was", open(os.path.join(e.installed("math"), "math.kh"), "rb").read() == good)
    e.cleanup()

    e = Env()
    os.remove(e.pkg_file("webi", "meta.kh"))
    rc, out = e.kh("install", "webi")
    check("a file that cannot be downloaded fails the install (it used to report success)", rc != 0 and "nothing was installed" in out, out)
    check("...and installs nothing", not os.path.exists(e.installed("webi")))
    e.cleanup()


# --------------------------------------------------------------------------- fail closed
def test_fail_closed():
    e = Env()
    del e.entry("math")["sha256"]
    e.save()
    rc, out = e.kh("install", "math")
    check("a registry entry without checksums is refused", rc != 0 and "no valid SHA-256" in out, out)
    check("the refusal tells the user about --no-verify", "--no-verify" in out, out)
    check("nothing was installed", not os.path.exists(e.installed("math")))
    rc, out = e.kh("install", "math", "--no-verify")
    check("--no-verify installs anyway", rc == 0 and e.is_installed("math"), out)
    check("--no-verify prints a warning", "WITHOUT checksum verification" in out, out)
    e.cleanup()

    e = Env()
    tamper(e.pkg_file("math"))
    rc, out = e.kh("install", "math", "--no-verify")
    check("--no-verify does not check (a tampered file installs)", rc == 0, out)
    e.cleanup()

    e = Env()
    del e.entry("webi")["sha256"]["routing.kh"]
    e.save()
    rc, out = e.kh("install", "webi")
    check("one missing checksum among many refuses the whole package", rc != 0 and "routing.kh" in out, out)
    check("...and installs nothing", not os.path.exists(e.installed("webi")))
    e.cleanup()

    for label, value in [("63 hex digits", "a" * 63), ("65 hex digits", "a" * 65), ("non-hex characters", "g" * 64), ("empty", ""), ("a number", 12345)]:
        e = Env()
        e.entry("math")["sha256"]["math.kh"] = value
        e.save()
        rc, out = e.kh("install", "math")
        check("malformed checksum (%s) is refused" % label, rc != 0, out)
        e.cleanup()

    e = Env()
    e.entry("math")["sha256"]["math.kh"] = e.entry("math")["sha256"]["math.kh"].upper()
    e.save()
    rc, out = e.kh("install", "math")
    check("upper-case hex checksum is accepted", rc == 0, out)
    e.cleanup()


# --------------------------------------------------------------------------- input validation
def test_validation():
    e = Env()
    e.entry("math")["url"] = "http://example.com/packages/math/math.kh"
    e.save()
    rc, out = e.kh("install", "math")
    check("plain http:// download URL from the registry is refused", rc != 0 and "not https" in out, out)
    e.entry("math")["url"] = "ftp://example.com/math.kh"
    e.save()
    check("ftp:// download URL is refused", e.kh("install", "math")[0] != 0)
    e.cleanup()

    e = Env()
    e.entry("webi")["files"] = e.entry("webi")["files"] + ["../evil.kh"]
    e.save()
    rc, out = e.kh("install", "webi")
    check("a ../ file name in the registry is refused", rc != 0 and "unsafe file name" in out, out)
    check("...and nothing was written outside the package directory", not os.path.exists(os.path.join(e.home, ".kosh", "evil.kh")))
    e.cleanup()

    e = Env()
    rc, out = e.kh("install", "math", env={"KH_REGISTRY_URL": "http://example.com/registry.json"})
    check("KH_REGISTRY_URL over plain http:// is refused", rc != 0 and "https:// or file://" in out, out)
    rc, out = e.kh("install", "math", env={"KH_RAW_BASE": "ftp://example.com/x"})
    check("KH_RAW_BASE with another scheme is refused", rc != 0, out)
    rc, out = e.kh("install", "../etc/passwd")
    check("path-traversal package name is refused", rc != 0 and "Invalid package name" in out, out)
    rc, out = e.kh("install", "math", "--bogus")
    check("an unknown option is refused", rc != 0 and "Unknown option" in out, out)
    rc, out = e.kh("install", "no_such_package_xyz")
    check("an unknown package is reported", rc != 0 and "not found" in out, out)
    e.cleanup()


# --------------------------------------------------------------------------- the real registry
def test_real_registry():
    reg = json.load(open(os.path.join(SRC_PACKAGES, "registry.json"), encoding="utf-8"))
    names = {p["name"] for p in reg["packages"]}
    problems = []
    for p in reg["packages"]:
        name = p["name"]
        shipped = [name + ".kh"] + p.get("files", [])
        if set(p.get("sha256", {})) != set(shipped):
            problems.append("%s: checksum keys != shipped files" % name)
        for f in shipped:
            path = os.path.join(SRC_PACKAGES, name, f)
            if not os.path.exists(path):
                problems.append("%s: %s missing on disk" % (name, f))
            elif p.get("sha256", {}).get(f) != hashlib.sha256(open(path, "rb").read().replace(b"\r\n", b"\n")).hexdigest():
                problems.append("%s: checksum of %s is stale" % (name, f))
        for d in p.get("dependencies", []):
            if d not in names:
                problems.append("%s: dependency %s is not in the registry" % (name, d))
    check("every package in the real registry ships all its files with a current checksum and valid dependencies", not problems, "\n       ".join(problems[:8]))


if __name__ == "__main__":
    for exe in (KH, KOSH):
        if not os.path.exists(exe):
            sys.exit("missing %s: run `make` first" % exe)
    for t in (test_sha256_command, test_good_installs, test_dependencies, test_tampering, test_fail_closed, test_validation, test_real_registry):
        t()
    print("  %d/%d passed   |   %d failed" % (passed, passed + failed, failed))
    sys.exit(1 if failed else 0)
