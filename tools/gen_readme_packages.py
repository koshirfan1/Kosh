#!/usr/bin/env python3
"""Regenerates the package tables in README.md and docs/packages.md from
packages/registry.json, so the count, versions and descriptions never go stale.

    python3 tools/gen_readme_packages.py           # rewrite the generated blocks
    python3 tools/gen_readme_packages.py --check   # exit 1 if they are out of date (CI)

Only the text between the BEGIN/END markers is touched:
    <!-- PACKAGES:BEGIN -->        ... README.md
    <!-- PACKAGES-TABLE:BEGIN -->  ... docs/packages.md
and the "packages-N-purple" badge count in README.md.

Every package in the registry must be listed in exactly one CATEGORY below, so
adding a package to registry.json without categorising it fails loudly.
"""
import json, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

CATEGORIES = [
    ("Core & CLI", ["math", "strings", "collections", "colors", "argparse", "validation", "dotenv", "uuid", "events", "logger", "test", "datetime"]),
    ("Web: the webi ecosystem", ["webi", "webi_auth", "webi_socket", "morgos"]),
    ("HTTP, APIs & testing", ["requests", "postman", "swagger", "openai"]),
    ("Data & storage", ["fs", "csv_io", "json_db", "orm", "sqlite"]),
    ("Computer vision & OCR", ["vision", "ocr"]),
    ("AI / ML utilities", ["tensor", "nn", "kbrain", "nlp", "quant"]),
    ("Networking & protocols", ["dns", "ftp", "grpc", "mqtt", "smtp", "ssh_client"]),
]
FEATURED = ["webi", "requests", "sqlite", "vision", "ocr", "tensor", "nn", "openai", "test"]
NOTES = {
    "sqlite": "needs libsqlite3 (dev headers) to build",
    "ocr": "opt-in: build with `make OCR=1` (needs libtesseract)",
}
DOCS = {"webi": "webi.md", "ocr": "ocr.md", "tensor": "tensor.md", "nn": "nn.md"}   # relative to docs/


def load():
    reg = json.load(open(os.path.join(ROOT, "packages", "registry.json"), encoding="utf-8"))["packages"]
    by = {p["name"]: p for p in reg}
    seen = {}
    for cat, names in CATEGORIES:
        for n in names:
            if n in seen:
                sys.exit("package %r is listed in two categories" % n)
            seen[n] = cat
    missing = sorted(set(by) - set(seen))
    gone = sorted(set(seen) - set(by))
    if missing:
        sys.exit("registry packages without a category in tools/gen_readme_packages.py: %s" % ", ".join(missing))
    if gone:
        sys.exit("categorised packages that are not in the registry: %s" % ", ".join(gone))
    return reg, by


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ").strip()


def short(desc, limit=100):
    """README table: keep the lead phrase (before the first colon) when the text is long."""
    desc = cell(desc)
    if len(desc) > limit and ":" in desc:
        desc = desc.split(":", 1)[0]
    if len(desc) > limit:
        desc = desc[:limit].rsplit(" ", 1)[0] + "..."
    return desc


def row(p, doc_prefix, brief=False):
    name = "`%s`" % p["name"]
    if p["name"] in DOCS:
        name = "[%s](%s%s)" % (name, doc_prefix, DOCS[p["name"]])
    desc = short(p["description"]) if brief else cell(p["description"])
    if p["name"] in NOTES:
        desc += " *(%s)*" % NOTES[p["name"]]
    return "| %s | %s | %s |" % (name, p["version"], desc)


HEAD = "| Package | Version | Description |\n|---|---|---|"


def readme_block(reg, by):
    lines = ["%d packages in %d categories, all installed the same way: `kh install <name>`, then `import \"<name>\"`." % (len(reg), len(CATEGORIES)), "", HEAD]
    for n in FEATURED:
        lines.append(row(by[n], "docs/", brief=True))
    lines += ["", "Also: " + ", ".join("`%s`" % n for _, names in CATEGORIES for n in names if n not in FEATURED) + ".", "",
              "Full list by category, with versions: [docs/packages.md](docs/packages.md)."]
    return "\n".join(lines)


def docs_block(reg, by):
    out = ["%d packages in %d categories." % (len(reg), len(CATEGORIES))]
    for cat, names in CATEGORIES:
        out += ["", "### " + cat, "", HEAD]
        out += [row(by[n], "") for n in names]
    return "\n".join(out)


def splice(text, begin, end, body):
    pat = re.compile(re.escape(begin) + r".*?" + re.escape(end), re.S)
    if not pat.search(text):
        sys.exit("markers %s ... %s not found" % (begin, end))
    return pat.sub(lambda m: begin + "\n" + body + "\n" + end, text, count=1)


def main():
    check = "--check" in sys.argv
    reg, by = load()
    targets = [
        ("README.md", "<!-- PACKAGES:BEGIN -->", "<!-- PACKAGES:END -->", readme_block(reg, by)),
        ("docs/packages.md", "<!-- PACKAGES-TABLE:BEGIN -->", "<!-- PACKAGES-TABLE:END -->", docs_block(reg, by)),
    ]
    stale = []
    for rel, b, e, body in targets:
        path = os.path.join(ROOT, rel)
        text = open(path, encoding="utf-8", newline="").read().replace("\r\n", "\n")
        new = splice(text, b, e, body)
        if rel == "README.md":
            new = re.sub(r"packages-\d+-purple", "packages-%d-purple" % len(reg), new)
            new = re.sub(r"and \d+ packages\.", "and %d packages." % len(reg), new)
        if new != text:
            stale.append(rel)
            if not check:
                open(path, "w", encoding="utf-8", newline="\n").write(new)
    if check:
        if stale:
            print("OUT OF DATE: " + ", ".join(stale) + "\nRun: python3 tools/gen_readme_packages.py")
            sys.exit(1)
        print("package tables are up to date (%d packages)" % len(reg))
    else:
        print("updated: " + (", ".join(stale) if stale else "nothing (already up to date)"))


if __name__ == "__main__":
    main()
