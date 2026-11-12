# Changelog

Versions are `0.x` until the roadmap's 1.0 criteria are met (see
`ROADMAP_STATUS_UPDATED.md`). Newest first.

## v0.016.1 - 2026-11-13

### Added
- **`kh` can now inspect what is installed**, like `pip`:
  `kh list` (installed packages and versions, offline), `kh list --outdated` / `kh outdated`,
  `kh show <name>` (files, requires, required-by), `kh update <name>` / `kh update --all`
  (verified, atomic, never downgrades), `kh check [name]` (missing files, files modified since
  install, missing dependencies; exit code 1 on problems), `kh freeze` (`name==version`),
  `kh search [term]`, and `kh uninstall` as an alias of `kh remove`.
- `kh install` records the file list, dependencies and verified SHA-256 of each file in the
  package's local `package.json` so the commands above work offline. Older installs still work
  (they just have no checksums for `kh check` to re-verify).
- 43 more checks in `tests/kh/test_kh_install.py` (119 in total), checked against deliberately
  broken builds and run under ASan/UBSan with leak checking.

### Changed
- **`kh list` now shows installed packages** (it used to list the registry). Browsing the
  registry moved to `kh search`. `kh installed` still works as an alias of `kh list`.
- `kh info` also says whether the package is installed, and whether an update is available.

### Fixed
- `kh remove` left every file except the main one and `package.json` behind for multi-file
  packages (`webi`, `vision`, `openai`, ...), so the package folder could never be deleted.
  It now removes the whole folder and warns if another installed package depends on it.
- `kh search` no longer cuts a UTF-8 character in half when it shortens a description.
- Added `.gitattributes` (`*.bat` as CRLF, `*.sh` as LF) so Windows checkouts stop showing
  `build.bat` as modified. Removed the stray `src/interpreter.h.bak`.

## v0.016.0 - 2026-10-27

### Security fixes
- **`kh install` now verifies every file** against a per-file SHA-256 published in the
  registry, downloads into a staging directory first, and installs nothing (leaving an
  existing install untouched) if any download, checksum or file name is bad. Missing or
  malformed checksums are refused (`--no-verify` overrides, with a warning). Previously a
  failed download of an extra file still reported success and left a half-installed package.
- **Heap buffer overflow** in the curl command builder used by `requests`
  (`http_request`) and webi on POSIX: every `'` in a URL or header expands to
  four bytes but only two were reserved. Found with AddressSanitizer;
  regression test `tests/regression_curl_quote_overflow.kh`.
- **`kh` package manager hardening:** package names, registry-supplied URLs
  and registry-supplied file names are validated before they reach a shell
  command or a filesystem path (no shell metacharacters, no `..`); truncated
  commands are refused.
- **Rate limiter** (`rate_limit_check`) could fail open: expired entries were
  never reused, so 1024 distinct keys over a server's lifetime disabled it
  permanently, and a full table allowed everything. Expired entries are now
  reused and, under a flood of live keys, low-count entries are evicted so
  keys already over the limit stay blocked. Regression test
  `tests/test_rate_limit.kh`.
- `crypto_lib.c` was never linked, so `sha256`, `password_hash` and
  `password_verify` did not exist at runtime. It is now built and registered;
  password salts come from the OS CSPRNG instead of `rand()`; the stored
  PBKDF2 iteration count is range-checked; webi's API-key comparison is
  constant-time.

### Added
- `kh` dependency installation: a package's `"dependencies"` are installed first
  (cycle-safe, depth-limited). Eight packages (`dns`, `nn`, `openai`, `orm`, `smtp`,
  `swagger`, `webi_auth`, `webi_socket`) imported another package that `kh install` never
  fetched, so they failed to import after a normal install.
- `kh sha256 <file>`, and `KH_REGISTRY_URL` / `KH_RAW_BASE` (https:// or file:// only) for
  mirrors and offline tests.
- `tools/gen_registry.py`: generates the registry's versions, file lists, dependencies and
  checksums from each package's `package.json` (CI fails on drift).
- `tests/kh/test_kh_install.py` (76 checks): tampering, truncation, missing/malformed
  checksums, atomic upgrades, dependencies, and that `webi`, `vision`, `openai`, `nn` import
  after a plain install.
- Native crypto builtins: `hmac_sha256`, `constant_time_equals`,
  `hkdf_sha256`, `x25519_public`, `x25519_shared`,
  `chacha20poly1305_encrypt`, `chacha20poly1305_decrypt` (RFC 8439 / 7748 /
  5869, no external library) and DNS helpers `dns_reverse`,
  `dns_resolves_to`.
- **webi 1.2.0 - encrypted channel** (opt-in, runs on top of TLS; read
  `docs/webi-threat-model.md` first): X25519 handshake, HKDF key schedule,
  ChaCha20-Poly1305 packets with counter nonces and a replay window
  (`secure.kh`, `secure_http.kh`); `require_secure` with a `public` path list;
  `bot_paths` served in the clear only to crawlers verified by
  forward-confirmed reverse DNS (`bot.kh`); a JavaScript client served from
  `/_webi/client.js` plus a `bootstrap` shell page for browsers
  (`client/webi.js`); an optional live self-check at `/_webi/proof`.
- `nn` and `quant` packages added to the registry.
- Tests: crypto, AEAD/X25519/HKDF (RFC and OpenSSL vectors), channel, HTTP
  integration, crawler verification, DNS, rate limiter, client serving, JS
  client (vectors, end-to-end against a real server, jsdom), and two
  deterministic fuzzers (`tests/fuzz/fuzz_webi.kh`,
  `tests/js/fuzz_webi_js.js`). See `docs/webi-testing.md`.
- CI: Linux added alongside Windows, an AddressSanitizer/UBSan run, and the
  new suites.
- `make OCR=1` builds the Tesseract OCR bridge (`src/ocr_lib.c`) and registers the
  `ocr_*` natives. It was not wired into the build at all before, so `import "ocr"`
  loaded but every call failed with `Undefined variable 'ocr_init'`. CI builds it and
  runs `tests/test_ocr.kh`.
- OCR test fixtures (`examples/ocr_test.png`, `ocr_paragraph.png`,
  `ocr_paragraph_rot90cw.png`, `ocr_french.png`), generated synthetic text; they were
  referenced by the test but never committed.
- `SECURITY.md`, `CONTRIBUTING.md`, and `tools/gen_readme_packages.py` (regenerates the
  package tables from `packages/registry.json`; CI fails on drift).

### Changed
- Registry repaired: `kh install webi` omitted the four new secure-channel files and
  `kh install vision` / `openai` omitted 11 and 9 sibling files, so those installs were
  incomplete; `tensor` was listed as 1.0.0 although the package is 2.0.0.
- 14 package manifests were saved as UTF-16 (a PowerShell `>` redirect artifact) and are
  now UTF-8; their `author` no longer names the suspended GitHub account.
- Rebrand finished: every remaining "Khan" is now "Kosh" (the author's name
  and personal links are unchanged); the playground WASM was patched in place
  (same-length rename) and should be rebuilt with `playground/build.sh`.
- `webi_version()` and the server `Server:` header report `webi/1.2.0`.
- Removed dead files (`src/khan.c`, `src/interpreter.h.bak`, duplicate
  `portfolio/` and `docs/RESULTS.md`).
- `tests/test_vision.kh` now uses a generated fixture
  (`tests/fixtures/sample.png`); it previously failed because
  `examples/photo.png` was never committed.
- README redesigned and shortened; the reference material moved to `docs/`
  (`install`, `packages`, `language`, `stdlib`, `architecture`, `status`, `roadmap`,
  `project-structure`). Corrected stale claims (package count, OCR being a required
  dependency, CI covering macOS).
- `docs/ocr.md` now describes OCR as an opt-in build.
- `Projects/portfolio` links to the current GitHub account.
- Removed an accidentally committed test artifact (`app.db`) and ignore `*.db`.

### Known limitations
- **No independent cryptographic or protocol review** of the channel; do not
  describe it as "100% secure". Fuzzing and the test suites find
  implementation bugs, not design flaws.
- The channel assumes TLS (the handshake is not authenticated on its own).
- Not yet exercised in real browsers (Chrome, Firefox, Safari) or on Windows.
- The JS client is not constant-time; script running in the page can read the
  session key. Text bodies only. Counters and sessions live in process memory.
- `kh install` verifies checksums, but the registry and the packages live in the same repository, so a compromised repository is not covered (that needs a signed registry).

## v0.015.0

Fresh-start release (new repository).
