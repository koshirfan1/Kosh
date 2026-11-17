# Changelog

Versions are `0.x` until the roadmap's 1.0 criteria are met (see
`ROADMAP_STATUS_UPDATED.md`). Newest first.

## v0.018.0 - 2026-11-18

Package security, phase A1 (from the community review of the non-webi packages). Each problem
below was reproduced first (many on a live process), then fixed, then covered by a test that fails
on the old code. **Several of these change behaviour on purpose**; see *Breaking*.

### Security
- **Six packages never did anything, and said they had.** `ftp`, `ssh_client`, `smtp`, `grpc`,
  `mqtt` and `webi_socket` printed a line and returned success (`ftp_upload()` returned `true`
  without uploading, `send_email()` returned `true` without sending, `ssh_exec()` returned
  `"SSH Output Placeholder"`), so a program could believe an email had gone out or a file had been
  uploaded. They are now marked **experimental** and every call reports failure (`false`, `nil`,
  or gRPC status `UNIMPLEMENTED`) with a `NOT IMPLEMENTED` message; each has `xxx_is_implemented()`.
  `send_email()` also no longer prints the recipient, subject and body. Their tests used to assert
  that the fake success was correct; they now assert the opposite.
- **`uuid` used a predictable generator.** IDs came from `random()`, seeded with the clock in whole
  seconds: 40 programs started in the same second produced the *same* "random" UUID (1 distinct
  id out of 40, measured). Every digit now comes from the system CSPRNG (`secure_token`), with no
  fallback to the weak generator. `uuid_is_valid()` accepted `"zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz"`;
  it now checks hex digits, the version and the variant, and `uuid_is_v4()` is new.
- **`webi_auth` rewritten (2.0.0).** Tokens were `token_<id>_<cpu clock>` (guessable) and `mw_auth`
  accepted *any* non-empty `Authorization` header. Tokens are now `kt1~user~expires~nonce~mac`,
  HMAC-SHA256 signed with your secret (32+ characters, from the environment), expire, carry a
  CSPRNG nonce, and are compared in constant time. `mw_auth` verifies them, rejects with a plain 401
  that does not say why, and with no secret configured rejects **everything** (fails closed).
  Also: secret rotation (`previous_secrets`), `webi_auth_public()` for the login route, case-insensitive
  `Bearer`, `auth_user(req)`. See `docs/webi-auth.md` for what it deliberately does not do
  (stateless tokens cannot be revoked; use short lifetimes and HTTPS).

### Added
- **Package status in `kh`.** `package.json` may say `"status": "experimental"` plus a
  `"status_note"`. `kh install` warns (also for a dependency); `kh search`, `kh list`, `kh show` and
  `kh info` mark it. `tools/gen_registry.py` copies the status into the registry and refuses an
  experimental package without a note or whose registry description does not start with
  `EXPERIMENTAL`. The README and `docs/packages.md` list experimental packages separately.
- `tests/security/test_packages.py`: process-level checks (simultaneous uuids; `webi_auth` on a real
  server: login, expiry, tampering, forged admin token, env secret, fails closed). The package tests
  for uuid, webi_auth and the six placeholders now run in CI (webi_auth's and the placeholders'
  tests were not run by CI before).

### Breaking
- `auth_create_token(user_id)` is gone: it needs `(user_id, secret, ttl_seconds)`. Tokens issued by 1.x
  are not valid. `mw_auth` needs `webi_auth_configure` / `webi_auth_secret_from_env`.
- `ftp_upload`, `ftp_download`, `mqtt_publish`, `mqtt_subscribe`, `send_email`, `ws_send`, `ws_broadcast`
  now return `false`; `ssh_exec` returns `nil`; `grpc_call` returns status `UNIMPLEMENTED`.
  Code that "worked" against the placeholders was not doing anything.
- `uuid_is_valid()` is stricter. `smtp` no longer depends on `requests`.

### Versions
`webi_auth` 2.0.0, `uuid` 1.1.0, `ftp` `grpc` `mqtt` `smtp` `ssh_client` `webi_socket` 1.0.1.

### Not yet done (phase A2 and later)
`requests`/`openai` hardening (found while reviewing, not fixed here): `file://` and other schemes
are accepted, a URL starting with `-` is parsed as a curl option, a malicious server can forge the
reported status, API keys are visible in `ps`, there is no request timeout (the `openai` timeout
setting is stored but never used), `build_query` does not URL-encode, and header values containing
CR/LF can inject headers. Then `fs`, `csv_io`, `dotenv`, `json_db`, `orm`, `logger`, `sqlite`,
`swagger`, `dns` (see the review notes in the project discussion).

## v0.017.0 - 2026-11-15

**Breaking change in webi 1.4.0: `render()` now HTML-escapes `{{key}}`.** This closes the
biggest remaining item of the community security review (cross-site scripting through templates)
and is the "auto-escaping by default" item of `docs/phase5-hardening-and-design-plan.md`. It gets
its own version because it can change what an existing app outputs.

### Changed (breaking)
- **`{{key}}` is escaped** (`&` `<` `>` `"` `'`), in `render()` and `render_file()`. Verified on a
  running server before the change: a request value `<script>alert(1)</script>` came out of
  `render()` verbatim.
- **`{{{key}}}` (three braces) inserts a value as-is**, for HTML you built yourself.
  `render_raw()` / `render_file_raw()` keep the old unescaped behaviour for a whole template
  (also available while migrating).
- **How to upgrade:** a placeholder that carries plain text needs no change (and is now correct:
  an `&` in a name used to produce broken markup). A placeholder that carries HTML you built:
  change `{{x}}` to `{{{x}}}`. Don't pass an already-escaped value to `{{x}}` (it would be
  escaped twice).

### Fixed
- **A value could read other template variables.** The old `render()` replaced one key after
  another over the whole string, so a user-supplied value containing `{{other}}` was expanded by a
  later key (order dependent). Rendering is now a single left-to-right pass; inserted values are
  never scanned again.
- `render()` was written in Kosh and called `str_replace` once per variable (see
  `docs/webi-performance-analysis.md`). It is now native C, linear in the template size, and
  rejects non-string templates / non-map variables with a clear error.

### Notes
- Escaping covers text and quoted attribute values. It does not make `<script>`/`<style>`
  content, unquoted or event-handler attributes, or URLs (`href="{{url}}"` with `javascript:`)
  safe; this is spelled out in `docs/webi.md`.
- Still open: the old per-request header parsing in `webi_handle` (duplicate headers,
  percent-decoding of query/form values).

### Tests
- `tests/test_webi_template.kh` (41 checks; 16 fail on the old `render()`), plus 4 checks on the
  wire in `tests/webi_http/test_http_hardening.py` (117 in total).

## v0.016.3 - 2026-11-15

webi production hardening (phase 2 of the community review). As before, each problem was
reproduced on a running server first, then fixed, then locked in with a test that fails on the
old behaviour. **Nothing here changes an app that does not opt in**, except the three fixes
marked *(behaviour change)*.

### Added
- **`webi_production(app, opts)`**: debug off (and `webi_debug(app, true)` refused), security
  headers on every response, a cookie policy, and CORS closed by default, in one call.
- **`webi_security_headers(app, opts)`**: `X-Content-Type-Options`, `X-Frame-Options`,
  `Referrer-Policy`, optional `Content-Security-Policy` (or report-only), `Permissions-Policy`,
  and `Strict-Transport-Security` (sent only for requests a TLS proxy marks as https). Applies to
  404s, middleware rejections and static files too; a header the handler set is never overwritten.
- **`webi_cookie_policy(app, opts)`**: adds `Secure`, `HttpOnly` and `SameSite` to any cookie that
  lacks them, with a `js_readable` list of cookie names that must not get `HttpOnly`.
- **`webi_limits(app, opts)`**: `max_header_bytes`, `max_body_bytes`, `read_timeout_seconds` and
  `request_timeout_seconds` are now configurable. Out-of-range or mistyped values stop the server
  from starting with a message instead of being ignored.
- **Whole-request timeout** (default 60 s). Before, only each single `recv` had a 30 s timeout, so
  a client sending a byte every 29 s could hold the single-threaded server for ever. The deadline
  is checked before every read, and also bounds how long the server waits on a client that
  stops reading its response (`SO_SNDTIMEO`).
- **`webi_api_keys_from_env(app, "VAR")`** loads API keys from an environment variable instead
  of source code; unset/empty fails closed. New native **`env_var(name, default)`**.
- Unknown options to any of these functions are reported (typo protection).

### Fixed
- **`webi_run_host(app, "127.0.0.1", port)` did not bind 127.0.0.1.** The host was ignored and the
  server always listened on every interface, so a "local-only" dev server was reachable from the
  network. The host is now honoured (IPv4 address, `localhost`, or `0.0.0.0`); an invalid host
  stops the server with a message. `webi_run()` still listens on every interface. *(behaviour change)*
- **`header_get()` was case-sensitive.** HTTP header names are case-insensitive, and proxies and
  HTTP/2 send them lower-cased, so `x-api-key`, `origin`, `cookie` and `x-csrf-token` could be
  missed: API-key auth rejected valid clients, and CORS/CSRF checks looked at the wrong thing.
  Lookup is now case-insensitive everywhere. *(behaviour change)*
- The CSRF token comparison used `==`; it is now constant-time (API keys already were).
- A request body is allocated as it arrives instead of `Content-Length` bytes up front, so a client
  claiming a huge body costs memory only for what it sends.
- `mw_cors` without `webi_set_cors_origins()` now prints a one-time warning that it allows every
  origin; in production mode the default is no cross-origin access. *(behaviour change in production mode only)*
- Debug mode combined with listening on every interface now prints a warning.

### Tests
- `tests/test_webi_production.kh` (67 checks) and 31 more socket-level checks in
  `tests/webi_http/test_http_hardening.py` (113 in total): limits, idle/trickle/slow-body timeouts,
  security headers and cookie flags on the wire, host binding, invalid configuration. Mutation
  checks caught every deliberately broken build; a weak timeout test found that way was rewritten.

### Notes
- webi is now `1.3.0`; `kh update webi` picks it up.
- Still not changed (next release, because it can change existing output): `render()` auto-escaping
  and its `{{value}}` re-substitution, and the old per-request header parsing in `webi_handle`
  (duplicate headers, percent-decoding of query/form values).

## v0.016.2 - 2026-11-13

webi HTTP-layer hardening (phase 1 of the review by a community reviewer). Every issue below
was first reproduced against a running server, then fixed, then locked in with a test that
fails on the old code.

### Security
- **Response-header injection.** A CR/LF in a value passed to `res_with_header()`,
  `res_with_cookie()` or `res_with_secure_cookie()` (for example a field from a JSON body)
  could add headers such as `Set-Cookie`. Those helpers now refuse such values, and the native
  server drops any header, cookie or Content-Type containing a control character, so a response
  map built by hand cannot bypass it. A cookie name/value/path with `;` is refused too, and
  `same_site` is validated. Handlers can no longer override `Content-Length`,
  `Transfer-Encoding` or `Connection`.
- **Strict request parsing** (new `wb_read_request_strict`, one shared reader for the POSIX and
  Windows servers). Refused with `400/413/414/431/501`: duplicate or non-numeric
  `Content-Length`, `Transfer-Encoding` (with `Content-Length` = 400, alone = 501), folded
  headers, header names that are not tokens, NUL/control characters/bare CR or LF, malformed or
  over-long request lines, more than 100 header lines, a body shorter than `Content-Length`,
  and NUL bytes in a body. Header names are matched exactly and case-insensitively:
  `CONTENT-LENGTH` now works, `X-Content-Length` is no longer mistaken for it.
- A client that disconnected in the middle of a response could kill the whole server
  (`SIGPIPE`). Sends now retry partial writes and never raise `SIGPIPE`.

### Fixed
- **Binary files were truncated.** `serve_static()` read files into a C string, so a PNG/JPG/PDF
  was cut at its first NUL byte (a 650-byte file was sent as 4 bytes). The native server now
  streams static files byte for byte from disk (up to 64 MB). Text types still fill `body`.
- **Long headers and cookies broke the response.** Headers were written through a 256-byte
  buffer, which cut off the line terminator of anything longer (a typical JWT cookie), merged
  the body into the headers and sent an empty body. Headers of any length are now written in full.
- A request body containing a NUL byte made the server wait for the 30 s timeout and answer
  with nothing; it is now an immediate `400`.
- Status `429` (and 408, 413, 414, 431, 501, ...) now carry their proper reason phrase instead of "OK".
- **Compiler:** `import "name"` crashed (a multi-exabyte `malloc`) when a *folder* called `name`
  sat next to the importing file, because folders were opened as if they were modules. Folders
  are now skipped, and the real package is found.

### Tests
- `tests/webi_http/test_http_hardening.py`: 82 checks against a real server over raw sockets,
  also run under ASan/UBSan with halt-on-error; checked against deliberately broken builds.
- `tests/fixtures/import_dir_shadow/check.kh` for the compiler fix. Both run in CI (Linux/macOS).

### Notes
- webi is now `1.2.1`. `kh update webi` picks it up.
- Not changed here, on purpose (next release, because it can change existing apps' output):
  `render()` auto-escaping, CORS and cookie defaults, security-headers middleware.

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
