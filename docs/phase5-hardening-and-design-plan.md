# Phase 5 plan — a real, secure, workable application stack for webi

Status: **planning only, not yet implemented.** This document is the
design; we build it step by step once you've reviewed it, the same way
Phase 4's plan (`docs/phase4-plan.md`) worked.

This is the third pass at this document, and each pass has widened the
scope for a good reason: v1 covered only anti-scanning defense. v2 added
the layers underneath it (transport, auth, secrets, supply chain). This
version adds what was still missing to call an application actually
**workable**, not just secure: a database, and the pieces (JWT, file
uploads) that depend on having real persistence. Security and capability
aren't separate tracks here — you can't secure a login system that has
nowhere to store a password, and a WAF blocking SQL injection attempts
means nothing if there's no SQL database for it to matter to yet.

Facts verified directly against the source, not assumed:
- **No database capability exists anywhere in Kosh.** No SQL, no driver,
  nothing. The only persistence mechanism today is raw `read_file()`/
  `write_file()` — which is exactly what the `.dat` files sitting in the
  project root (`todos.dat`, `test_todos.dat`, `test2.dat`) are: ad hoc
  flat-file storage, because there's currently nothing better available.
- **No file-upload handling exists.** `form_get()` in `request.kh` only
  parses URL-encoded bodies (`key=value&key2=value2`); there's no
  multipart/form-data parsing anywhere, so an app literally cannot
  accept an uploaded file today.
- **`json_lib.c`'s parser has no recursion depth limit.** Untrusted
  request bodies go straight through `json_decode()` with no guard —
  deeply nested JSON is a working stack-overflow crash today.
- **No native for reading OS environment variables** is exposed to Kosh
  scripts, which matters for keeping secrets (including the JWT signing
  key and database credentials, once both exist) out of source files.

---

## Part 0 — what this document is actually promising

Same honest framing as before, worth keeping every time this doc grows:
no system is unhackable, and "most secure" only ever means genuine
defense-in-depth across every layer a request passes through — never a
single silver bullet. What's new in this version is that some of those
layers (database, auth) aren't just about security anymore — they're
what makes webi capable of running a real application at all, not a
routing demo. Both goals point at the same missing pieces, which is why
they belong in one document.

---

## Layer 1 — Transport security (TLS)

Unchanged from the previous version. `webi`'s native server is plain
HTTP only — every cookie, token, and form field goes over the wire in
cleartext today. Recommended path: document and default to a
TLS-terminating reverse proxy (Caddy/nginx/platform load balancer) —
low risk, zero new C code, how most small frameworks are actually
deployed. Native TLS via a vetted library (mbedTLS) is a real but
higher-risk stretch goal, never a hand-rolled implementation.

---

## Layer 2 — Database

This is the biggest addition in this revision, and arguably the most
important one — without it, "application" is the wrong word for what
webi can build today.

**Engine choice: SQLite, vendored directly into the build.** Not
Postgres/MySQL, and deliberately so:
- SQLite ships as a single-file amalgamation (`sqlite3.c`/`sqlite3.h`)
  that compiles straight into `kosh.exe` alongside the existing source
  files — no new runtime dependency, no separate database server
  process to install/run/secure, works identically on Windows/Linux/macOS
  the same way the rest of Kosh already does (this matters concretely —
  the actual development machine in this project is Windows, where
  installing and linking against `libpq`/`libmysqlclient` is genuinely
  painful; SQLite sidesteps that completely).
- It's a real, production-grade SQL database (not a toy) — plenty of
  real applications run on SQLite alone, and it's a well-tested,
  extremely widely deployed piece of software (it's already on every
  phone and most browsers).
- A generic driver abstraction (so Postgres/MySQL could be added later
  as additional backends behind the same Kosh-level functions) is worth
  keeping in mind while designing the API, but is explicitly **not**
  part of this phase — one working, secure database story beats two
  half-finished ones.

**New natives** (in a new `src/db_lib.c`):
- `db_open(path) -> handle` — opens/creates a SQLite file
- `db_exec(handle, sql, params) -> bool` — for `INSERT`/`UPDATE`/`DELETE`/DDL
- `db_query(handle, sql, params) -> array of row maps` — for `SELECT`,
  returning each row as a Kosh map keyed by column name (same shape
  developers already expect from working with `req`/`res` maps)
- `db_close(handle)`

**Parameterized queries are not optional — this is the actual fix for
SQL injection, not `waf_scan()`.** The most important design decision
here: `params` is a *separate array argument*, bound via SQLite's
prepared-statement API (`sqlite3_bind_*`), never string-concatenated
into the SQL text. This needs to be true structurally, not just
documented as a best practice — if `db_exec`/`db_query` only accepted a
single pre-built SQL string, every app built on webi would be one
`"SELECT * FROM users WHERE name = '" + input + "'"` away from SQL
injection, and Layer 6's WAF would be the *only* thing standing in the
way — which is exactly backwards. **The WAF is defense-in-depth on top
of parameterized queries, never a substitute for them.** This needs to
be stated explicitly in the actual API docs when this ships, not just
here.

**Migrations** — once there's a real schema, there needs to be a way to
change it over time without manually editing the database file by hand.
Simplest workable version: a `migrations/` folder of numbered `.sql`
files and a `db_migrate(handle, folder)` that applies whichever ones
haven't run yet, tracked in a small internal table SQLite manages for
us. Not a full migration framework (branching, rollback) — just enough
to version schema changes safely, which is the actual need.

**Connection lifecycle in the server loop** — `db_open()` happening once
at app startup (not per-request) is the obvious model; worth confirming
whether SQLite's threading mode needs any special handling once Phase 4
(threaded server) exists, since that's a real interaction between two
things in this plan that both touch concurrency.

---

## Layer 3 — Authentication: sessions and JWT, as complementary options

Expanded from the previous version to cover JWT explicitly, since a
database existing now makes a full auth story possible, not just a
sketch of one.

**Password hashing** (unchanged from before): `password_hash(plain) ->
hash` / `password_verify(plain, hash) -> bool`, PBKDF2-HMAC-SHA256 with
a per-password random salt and a high iteration count. This needs a real
`sha256`/`hmac_sha256` primitive in native code — worth building that as
a shared internal building block, because JWT signing below needs the
exact same HMAC-SHA256 primitive. One correct implementation, two
consumers, rather than two.

**Sessions vs JWT — pick based on what's actually being built, not one
"correct" answer:**

| | Server-side sessions | JWT |
|---|---|---|
| Where state lives | Server (the DB, now that one exists) | Inside the token itself |
| Revoking access immediately | Easy — delete the session row | Hard — the token is valid until it expires, unless a revocation list is *also* kept server-side (which gives up JWT's main advantage) |
| Good fit for | The browser-facing web app itself | A separate API surface — mobile clients, third-party integrations, service-to-service calls |
| Needs | `res_with_secure_cookie()` (already built) + a `sessions` DB table | `jwt_encode`/`jwt_decode` natives + a signing secret from `env_get()` (Layer 4) |

Realistic plan: **build both**, since a real application commonly wants
cookie-based sessions for its own web pages and JWT for an API it
exposes — they're not competing choices, they solve different problems.

**JWT specifics** (new natives, `jwt_encode(payload, secret, expiry_seconds)
-> token`, `jwt_decode(token, secret) -> payload | nil`):
- Standard structure: `base64url(header) + "." + base64url(payload) +
  "." + base64url(HMAC-SHA256(header + "." + payload, secret))`
- **The classic, well-documented JWT vulnerability class that this
  implementation must not repeat: never trust an `alg` field read from
  the token itself** (the historical "`alg: none`" attack — a forged
  token claims it isn't signed at all, and a naive verifier that reads
  the algorithm from the token believes it). This implementation should
  hard-code HMAC-SHA256 as the only supported algorithm, full stop, and
  never branch on anything the token itself claims about how it was
  signed.
- `exp` (expiry) claim checked on every decode; an expired token decodes
  to `nil`, same "fail closed, don't raise" pattern already used by
  `render_file()`'s rejections.
- The signing secret comes from `env_get()` (Layer 4) — never hardcoded
  in a `.kh` file, for the same reason any other secret shouldn't be.

**Still true from the previous version, unchanged:** session fixation
prevention (new session ID issued on login, not reused), and
per-account brute-force lockout independent of the existing per-IP rate
limiting.

---

## Layer 4 — Input handling & output safety

Unchanged from the previous version, with one addition:

- Auto-escaping by default in `render()`/`render_file()` (`{{var}}`
  escapes, an explicit `{{{var}}}` opts out) — still the single highest-leverage
  XSS fix, still a breaking change worth its own changelog note.
- JSON parser recursion depth limit — still a live, verified bug, still
  small and worth fixing early and in isolation.
- WAF patterns stay non-backtracking (no ReDoS).
- **New, tied to Layer 2:** every `db_query`/`db_exec` call in any example
  or documentation this project ships needs to use parameterized calls,
  with zero exceptions — since the very first SQL example anyone copies
  from webi's own docs sets the pattern everyone else follows.

---

## Layer 5 — File uploads

New in this revision — a real gap, and it's specifically a *webi*
feature (native + Kosh), not a database feature, though it's what makes
storing an uploaded file somewhere (the database, or disk via
`write_file()`) actually possible.

- **Multipart/form-data parsing** — currently entirely absent. New
  native parsing in `webi_lib.c` (or a new `multipart_lib.c`) that reads
  a `multipart/form-data` body, splits it on its boundary, and exposes
  each part as a Kosh map: `{"name": "avatar", "filename": "photo.jpg",
  "content_type": "image/jpeg", "data": "<raw bytes>"}`.
- **Needs its own size limits**, separate from the existing whole-body
  cap (v1.1.1's `WB_MAX_BODY_SIZE`) — a per-file size limit and a
  maximum file count per request, so "unlimited file upload size" isn't
  a new denial-of-service vector reopening what Layer already closed
  for request bodies generally.
- **Content-type/extension validation on upload** is an application-level
  decision (an image-upload endpoint should reject a `.exe`), not
  something webi should hardcode — but webi should make the check easy
  to write correctly (exposing the real detected content type, not just
  trusting the filename extension the client sent, since that's
  attacker-controlled).
- Ties directly to Layer 2/render_file's binary-file limitation already
  documented in Phase 3 (`serve_static()`'s known NUL-byte-truncation
  note) — worth resolving both at the same time, since real file uploads
  (images especially) make that limitation matter a lot more than static
  CSS/JS assets did.

---

## Layer 6 — Secrets & configuration security

Unchanged: `env_get(name, default) -> string` native, `.env`-file
convention, documented `.gitignore` recommendation. Matters more now
that there are two real secrets to keep out of source (the JWT signing
key, and — once non-SQLite backends ever exist — database credentials).

---

## Layer 7 — Supply chain security

Unchanged: `kh.c`'s TLS/cert handling is already fine (verified), but
there's no integrity check on downloaded package content — add
`sha256` verification per file in `registry.json`, checked before
anything is written to disk.

---

## Layer 8 — Anti-scanning / automated-attack defense

Unchanged in substance from the previous version — security headers,
`waf_scan()` + `mw_waf`, scanner UA fingerprinting, behavioral scoring +
auto-ban, honeypot routes, response-consistency audit, RBAC hook. See
the previous revision's detail; not repeated in full here since nothing
about it changed, only its position in the overall document (now Layer
8 of 11, correctly framed as one layer among several rather than the
whole story). One addition specific to this revision: SQL-injection
patterns in `waf_scan()` remain valuable as defense-in-depth **on top
of** Layer 2's parameterized queries, not instead of them.

---

## Layer 9 — Audit logging & incident visibility

Unchanged: a structured, separate security event log (one parseable
record per line), never logging raw untrusted payloads verbatim.

---

## Layer 10 — Secure-by-default ergonomics

Unchanged: `webi_harden(app)` as one call enabling sensible defaults
together, since most real breaches come from a protection existing but
never being turned on, not from every layer being individually defeated.

---

## Layer 11 — Design system ("most beautiful")

Unchanged: bundled base theme, pre-built HTML partials, light/dark mode
from the start, served via `serve_static()`. Independent of everything
else in this document, can be built in parallel.

---

## What's native (C) vs pure Kosh

| Piece | Where |
|---|---|
| SQLite integration (`db_open`/`db_exec`/`db_query`/`db_close`) | native — vendors SQLite's amalgamation |
| `sha256`/`hmac_sha256` shared primitive | native — used by both password hashing and JWT |
| `password_hash()`/`password_verify()` | native, built on the primitive above |
| `jwt_encode()`/`jwt_decode()` | native, built on the same primitive |
| Multipart/form-data parsing | native (`webi_lib.c` or new `multipart_lib.c`) |
| Session table (create/get/destroy/expire) | native — same architecture as `rate_limit_check()` |
| JSON parser depth limit | native — inside `json_lib.c`'s existing recursive functions |
| `env_get()` | native — thin `getenv()` wrapper |
| Package checksum verification | native (`kh.c`) |
| `waf_scan()` + behavioral scoring table | native |
| Migrations runner | could be pure Kosh (`db_migrate()` reading `.sql` files + calling `db_exec`) |
| Auto-escaping in templates | pure Kosh (`template.kh`) |
| Security headers, honeypots, RBAC hook, `webi_harden()` | pure Kosh (`security.kh`) |
| Structured security log formatting | pure Kosh |
| Design system files | pure Kosh/CSS/HTML |

---

## Suggested build order

| Step | What | Depends on |
|---|---|---|
| 1 | JSON recursion depth limit | — (live bug, small, isolated) |
| 2 | `env_get()` native | — |
| 3 | SQLite integration (`db_lib.c`) + parameterized queries | — |
| 4 | Migrations runner | step 3 |
| 5 | Shared `sha256`/`hmac_sha256` native primitive | — |
| 6 | Password hashing | step 5 |
| 7 | Sessions (DB-backed now that step 3 exists) | steps 3, 6 |
| 8 | JWT encode/decode | steps 2 (signing secret), 5 |
| 9 | Per-account brute-force protection | step 7 or 8 |
| 10 | Multipart/file-upload parsing | — |
| 11 | Auto-escaping in templates | — (breaking change, own changelog note) |
| 12 | Security headers middleware | — |
| 13 | `waf_scan()` native + `mw_waf` | step 1's discipline (no backtracking regex) |
| 14 | Scanner UA fingerprinting | — |
| 15 | Behavioral scoring + auto-ban native | steps 13/14 |
| 16 | Honeypot routes | step 15 |
| 17 | Package checksum verification (`kh.c`) | — |
| 18 | Structured security logging | steps 13-16 |
| 19 | Response-consistency audit | steps 12-16 |
| 20 | `webi_harden(app)` | steps 11-16 |
| 21 | RBAC hook | independent |
| 22 | TLS (reverse-proxy docs first; native TLS as a separate stretch goal) | independent |
| 23 | Design system | independent, parallel with everything |

Version bump: **1.2.0**.

---

## One more honest note

This document now covers two things that are related but not identical:
making webi **secure**, and making webi **capable of running a real
application** — a database and file uploads aren't security features,
they're the missing floor underneath everything else. They earned their
place here because half of this document's security layers (sessions,
JWT, per-account lockout) don't fully make sense without them, and
because building them at the same time means the very first examples
this project ships (a login system, a file-upload endpoint) get built
the secure way from day one, instead of a fast version now and a
security pass "later" that may or may not happen. That's the actual
goal of combining these into one plan rather than two.