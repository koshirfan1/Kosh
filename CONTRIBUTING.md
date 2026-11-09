# Contributing to Kosh

Thanks for helping. This file is the short version of how to build, test and
send a change that is easy to review.

## Build

You need GCC (C11), GNU Make and the libsqlite3 development headers
(`apt install libsqlite3-dev`, `brew install sqlite3`, or MSYS2
`mingw-w64-x86_64-sqlite3`).

```bash
make                # builds kosh and kh
make OCR=1          # optional: also builds the Tesseract OCR bridge (needs libtesseract, see docs/ocr.md)
make clean
```

More detail, including Windows: [docs/install.md](docs/install.md).

## Test

```bash
./kosh tests/run_all.kh                 # the core assertion suite
./kosh tests/test_crypto.kh             # one suite; every tests/test_*.kh runs the same way
./kosh tests/fuzz/fuzz_webi.kh 1 1000   # Webi channel property fuzzer: seed, iterations
node tests/js/test_webi_js_vectors.js   # browser client vectors (needs Node 18+)
python3 tests/fuzz/negative_cases.py ./kosh
python3 tests/kh/test_kh_install.py     # kh install: checksums, dependencies, atomic installs (needs curl)
```

`tests/test_ocr.kh` needs a binary built with `make OCR=1` and the `tesseract-ocr-eng`
and `tesseract-ocr-fra` language packs. The Webi suites are described in
[docs/webi-testing.md](docs/webi-testing.md).

For anything that touches C code, also run the suites under AddressSanitizer and
UBSan, the way CI does:

```bash
SRCS=$(sed -n '/^SRCS/,/src\/main.c/p' makefile | grep -o 'src/[a-z_0-9]*\.c' | tr '\n' ' ')
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -Isrc -D_POSIX_C_SOURCE=200809L -w $SRCS -o kosh_asan -lm -lsqlite3
ASAN_OPTIONS=detect_leaks=0 ./kosh_asan tests/run_all.kh
```

## Generated files: do not edit by hand

| File | Regenerate with |
|---|---|
| `packages/webi/client_js.kh` (the browser client served by webi) | `python3 tools/gen_client_js.py` after editing `packages/webi/client/webi.js` or `proof.js` |
| `packages/registry.json` versions, file lists, dependencies and SHA-256 checksums | `python3 tools/gen_registry.py` after changing any shipped package file or its `package.json` |
| The package tables in `README.md` and `docs/packages.md` | `python3 tools/gen_readme_packages.py` after changing `packages/registry.json` |

CI runs a drift check for both, so a forgotten regeneration fails the build.

## Adding a package

1. Create `packages/<name>/<name>.kh` and `packages/<name>/package.json` (UTF-8; list every extra file under `"files"` and any other package you `import` under `"dependencies"`).
2. Add an entry to `packages/registry.json` with `name`, `description`, `developer`, `author` and `url` (ending in `/<name>/<name>.kh`), then run `python3 tools/gen_registry.py` to fill in the version, files, dependencies and checksums.
3. Add the package to a category in `tools/gen_readme_packages.py`, then run it.
4. Add tests under `tests/` and a short doc under `docs/` if it is more than a few functions.

## Pull requests

- One topic per pull request, with a clear description of what changed and why.
- **Fixing a bug? Add a regression test** that fails without the fix (see the `tests/regression_*.kh` files).
- Keep claims honest. If you add a feature with a known gap, say so in the docs;
  this project's README makes a point of not rounding up.
- Security-sensitive changes (crypto, parsing of network input, shell commands)
  should say what was tested and how. To report a vulnerability, use [SECURITY.md](SECURITY.md)
  instead of a public issue.
- Update [CHANGELOG.md](CHANGELOG.md) under "Unreleased".

## Code style

C11, no external dependencies unless they are opt-in (like OCR and LLM). Follow the
style of the file you are editing. Kosh code uses 4-space indentation.
