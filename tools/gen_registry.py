#!/usr/bin/env python3
"""Keeps packages/registry.json consistent with the package folders and adds the
per-file SHA-256 checksums that `kh install` verifies.

    python3 tools/gen_registry.py           # rewrite packages/registry.json
    python3 tools/gen_registry.py --check   # exit 1 if it is out of date (CI)

For every registry entry the package's own packages/<name>/package.json is the
source of truth for:
    version       copied to the registry entry
    dependencies  other registry packages this one imports; `kh install` installs
                  them first. Checked against the package's real `import "..."` lines.
    files         the extra files to ship next to <name>.kh (top-level files only; a
                  path with a "/" is a source file, e.g. webi's client/webi.js, and
                  is not shipped separately)
and every shipped file (the main file plus those extras) gets an entry in
    "sha256": { "<file>": "<64 hex>", ... }
The checksum is of the bytes GitHub will serve: the git index content, or, when the
working copy has unstaged edits, the working file with CRLF normalised to LF. Windows
line-ending conversion in a working copy therefore cannot change it.

Run this after changing any shipped package file, then commit the registry. CI
fails if you forget. The registry's description, developer, author and url are
hand-maintained and never touched here.
"""
import hashlib, json, os, re, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
REG = os.path.join(ROOT, "packages", "registry.json")


def read_json(path):
    raw = open(path, "rb").read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        sys.exit("%s is UTF-16; save it as UTF-8 (PowerShell's `>` redirect does this)" % os.path.relpath(path, ROOT))
    return json.loads(raw.decode("utf-8-sig"))


def blob(relpath):
    """Bytes GitHub will serve for this file once committed.

    Normally that is the git index content (exactly what is stored). If the working
    copy differs from the index (an edit that is not staged yet), the working copy is
    used instead, with Windows CRLF line endings normalised to LF, so the result does
    not depend on whether you ran `git add` first.
    """
    work = open(os.path.join(ROOT, relpath), "rb").read()
    norm = work.replace(b"\r\n", b"\n")
    r = subprocess.run(["git", "show", ":" + relpath.replace(os.sep, "/")], cwd=ROOT, capture_output=True)
    if r.returncode == 0 and r.stdout.replace(b"\r\n", b"\n") == norm:
        return r.stdout
    return norm


def imported_packages(pdir, names, self_name):
    """Registry packages imported (not as sibling .kh files) anywhere in this package folder."""
    found = set()
    for root, _, files in os.walk(os.path.join(ROOT, pdir)):
        for f in files:
            if not f.endswith(".kh"):
                continue
            for line in open(os.path.join(root, f), encoding="utf-8", errors="replace"):
                m = re.match(r'\s*(?:import|from)\s+"([^"]+)"', line)
                if m and not m.group(1).endswith(".kh") and m.group(1) in names and m.group(1) != self_name:
                    found.add(m.group(1))
    return found


def find_cycle(deps):
    state = {}

    def visit(n, path):
        if state.get(n) == 2:
            return None
        if state.get(n) == 1:
            return path + [n]
        state[n] = 1
        for d in deps.get(n, []):
            c = visit(d, path + [n])
            if c:
                return c
        state[n] = 2
        return None

    for n in deps:
        c = visit(n, [])
        if c:
            return " -> ".join(c)
    return None


def build():
    doc = read_json(REG)
    problems = []
    names = {p["name"] for p in doc["packages"]}
    all_deps = {}
    for p in doc["packages"]:
        name = p["name"]
        pdir = os.path.join("packages", name)
        main = name + ".kh"
        if not p["url"].endswith("/%s/%s" % (name, main)):
            problems.append("%s: url must end with /%s/%s" % (name, name, main))
        manifest = os.path.join(ROOT, pdir, "package.json")
        if not os.path.exists(manifest):
            problems.append("%s: no package.json" % name)
            continue
        pj = read_json(manifest)
        p["version"] = pj["version"]
        extra = [f for f in pj.get("files", []) if f != main and f != "package.json" and "/" not in f]
        if extra:
            p["files"] = extra
        else:
            p.pop("files", None)
        declared = pj.get("dependencies", [])
        for d in declared:
            if d not in names:
                problems.append("%s: dependency %r is not in the registry" % (name, d))
        used = imported_packages(pdir, names, name)
        if used - set(declared):
            problems.append("%s: imports %s but package.json 'dependencies' does not list it (kh would install a broken package)" % (name, sorted(used - set(declared))))
        if declared:
            p["dependencies"] = declared
        else:
            p.pop("dependencies", None)
        all_deps[name] = declared
        shipped = [main] + extra
        hashes = {}
        for f in shipped:
            rel = os.path.join(pdir, f)
            if not os.path.exists(os.path.join(ROOT, rel)):
                problems.append("%s: shipped file %s does not exist" % (name, f))
                continue
            hashes[f] = hashlib.sha256(blob(rel)).hexdigest()
        p["sha256"] = hashes
        loose = sorted(f for f in os.listdir(os.path.join(ROOT, pdir)) if f.endswith(".kh") and f not in shipped)
        if loose:
            print("note: %s has .kh files that are not shipped (not in package.json 'files'): %s" % (name, ", ".join(loose)))
    cyc = find_cycle(all_deps)
    if cyc:
        problems.append("dependency cycle: " + cyc)
    return doc, problems


def main():
    check = "--check" in sys.argv
    doc, problems = build()
    if problems:
        print("\n".join(problems))
        sys.exit(1)
    new = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    old = open(REG, encoding="utf-8").read().replace("\r\n", "\n")
    if new == old:
        print("registry is up to date (%d packages)" % len(doc["packages"]))
        return
    if check:
        print("packages/registry.json is OUT OF DATE (versions, dependencies, file lists or checksums changed).\n"
              "Run: python3 tools/gen_registry.py   and commit the result.")
        sys.exit(1)
    open(REG, "w", encoding="utf-8", newline="\n").write(new)
    print("updated packages/registry.json (%d packages)" % len(doc["packages"]))


if __name__ == "__main__":
    main()
