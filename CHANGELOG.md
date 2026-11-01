# Changelog

Versions are `0.x` until the roadmap's 1.0 criteria are met (see
`ROADMAP_STATUS_UPDATED.md`). Newest first.

## v0.016.0 - 2026-10-27

### Security fixes
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
- `kh install` still does not verify checksums of downloaded packages.

## v0.015.0

Fresh-start release (new repository).
