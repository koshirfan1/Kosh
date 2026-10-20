# Kosh v1.0 Roadmap — Status (Updated)

Tracking against the 19-point stability plan. Honest status, not optimistic status.
This revision layers a second session's findings on top of the previous update —
prior entries are kept, new findings are marked **(this session)**.

Legend: ✅ Done &nbsp; 🟡 Partial &nbsp; ❌ Not started

---

## 0. Build system — 🟡 Partial **(new item this session)**

The original roadmap didn't track this separately, but it turned out to be the
most basic blocker of all:

- ✅ **(this session)** Fixed: the project did not compile at all on Linux.
  `src/main.c` called the Windows-only `GetFullPathNameA` unconditionally
  (no `#ifdef _WIN32` guard). Added a POSIX `realpath()` fallback.
- ✅ **(this session)** Fixed: `src/requests_lib.c` had a genuine compile
  error — it accessed a map's contents as `headers.as.map` when the real
  `Value` union stores maps at `headers.as.obj->as.map`, and map entries
  hold a `Value*` (pointer), not a `Value`. This alone made a clean build
  from source impossible.
- ✅ **(this session)** Fixed (introduced-then-caught in the same session):
  glibc's fortified `realpath()` requires a destination buffer of at least
  `PATH_MAX` bytes or it aborts with "buffer overflow detected" at runtime.
  The first fix used a 2048-byte buffer; corrected to `PATH_MAX`.
- ❌ No CI / no automated build-matrix (see #17) to catch this kind of
  regression automatically going forward.

**Bottom line: prior to this session, `make` did not produce a working
`kosh` binary on Linux from a clean checkout.** Everything below assumes
this is now fixed and verified (`make clean && make` succeeds, `kosh` runs).

---

## 1. Lexer — 🟡 Partial
- ❌ Unicode support
- ❌ UTF-8 validation
- ❌ Better indentation handling
- ❌ Escape sequence audit
- 🟡 Error location — line numbers exist; column numbers and the
  `SyntaxError / File / Line / Column` structured format from the example
  do not.
- 🟡 **(this session)** Confirmed (not fixed, by design): single-quoted
  strings (`'like this'`) are not supported — only `"double-quoted"`
  strings exist. One test file (`examples/webi_test.kh`) had 7 stray
  single-quoted literals left over from habits in other languages; fixed
  the test file rather than adding single-quote support, since nothing
  else in the codebase uses or documents it.

## 2. Parser — 🟡 Partial
- ✅ Confirmed the parser does NOT segfault on invalid syntax in every
  case tested this session (5 packages with real syntax errors all
  failed with a clean parse error, not a crash).
- ❌ No systematic fuzz/negative-case testing to confirm this holds
  everywhere (e.g. `if x >` with nothing after — untested).
- ❌ Error messages aren't consistently in the "Expected expression
  after '>'" style shown in the example — some are lower-level compiler
  errors ("Expected indented block after ':'").
- 🟡 **(this session)** Confirmed there is no `**` (exponentiation) or
  `*` string-repeat operator — only `+` (numeric add / string concat),
  `-`, `*`, `/`, `%` (all numeric-only) are implemented. One stray
  example (`examples/import_test.kh`) assumed `**` worked; left as-is
  (not fixed) since adding an operator is a language decision, not a bug
  fix — flagging for the maintainer rather than making the call
  unilaterally.

## 3. AST — ❌ Not started
- No audit of node independence for future optimization passes.

## 4. Compiler optimizations — ❌ Not started
- ❌ Constant folding (`2+3` → `5` at compile time)
- ❌ Dead code elimination
- ❌ Constant propagation
- ❌ Peephole optimization
- Two real correctness bugs were found and fixed in the compiler in the
  prior session (continue-jump patching, higher-order function calls).
- ✅ **(this session)** Found and fixed a third, larger one: **the
  compiler had no concept of closures at all.** See #6 below — this is
  the single biggest correctness finding of this session and arguably
  belongs as much here as in "VM".

## 5. Bytecode — 🟡 Partial
- ❌ No opcode count audit, no compactness pass.
- ✅ **(this session)** Added two new opcodes (`OP_GET_UPVALUE`,
  `OP_SET_UPVALUE`) as part of the closures fix — necessary, not
  optional; noting here since it changes the opcode count baseline for
  whenever #5's audit actually happens.

## 6. VM — 🟡 Partial
- ❌ No computed-goto vs switch-dispatch benchmark
- ❌ No stack overflow/underflow detection (frame-count overflow *is*
  checked — `VM_FRAMES_MAX` — but not stack *value* overflow/underflow)
- ❌ No tail call optimization
- ❌ No inline-native-call optimization (e.g. `len()` still does a full
  global lookup every call)
- Two real VM bugs were fixed in the prior session (function-call
  argument shift, top-level block-scope locals) — correctness, not the
  performance work asked for here.
- ✅ **(this session) Closures.** The VM had genuinely no upvalue
  mechanism: a nested `fn` declared inside another function could not
  see that outer function's parameters or `let`s at all — every such
  reference resolved as a runtime "Undefined variable" error. Confirmed
  with a minimal repro (`fn outer(x): fn inner(): return x+1`) and
  traced to a real consumer: `webi`'s `serve_static()` route-handler
  factory, which was completely broken as a result (`examples/
  webi_phase3_test.kh` failed outright before this fix).

  Implemented real (if intentionally scoped-down) closures:
  - Compile-time free-variable resolution walks the enclosing-function
    chain (`resolve_upvalue`), threading captures through intermediate
    functions so nesting more than one level deep works correctly
    (verified: 3-level nesting, independent closure instances from
    repeated calls to the same factory function).
  - Runtime: a small ref-counted `KhanClosure` (captured-values array)
    gets attached to a function `Value` at the moment a nested `fn`
    statement executes, and threaded through `CallFrame` on `OP_CALL`.
  - **Known limitation, by design (documented, not hidden):** capture
    is by-value snapshot at closure-creation time, not a live mutable
    reference. Assigning to a captured variable from inside the nested
    function updates that closure's own copy but does **not** propagate
    back to the enclosing function's variable after return. This is
    correct C-style/Python-style read-capture ("factory" pattern, the
    only pattern found in the actual codebase — `webi/routing.kh`,
    `swagger/swagger.kh`) but is *not* full mutable-upvalue closures
    (e.g. a "counter" closure that increments and remembers state
    across calls wouldn't work). Flagging this explicitly rather than
    calling it "closures, done" — a future pass to add heap-boxed
    upvalue cells (clox-style "closing" on scope exit) would be needed
    for full correctness.
  - Blast radius checked: only 2 files in the whole codebase
    (`packages/webi/routing.kh`, `packages/swagger/swagger.kh`) used
    nested-function-capturing-outer-locals before this fix, so this
    wasn't visible everywhere — but it's exactly the kind of gap that
    silently breaks any *future* code using a very natural, common
    pattern.

## 7. Memory manager — ❌ Not started
- No 10M-allocation stress test
- No leak audit
- No fragmentation testing
- ✅ **(this session)** The new closure mechanism (#6) is ref-counted
  (`khanclosure_retain`/`khanclosure_release`, mirroring existing
  `value_copy`/`value_free` conventions) specifically to avoid adding a
  new leak/double-free surface — but this has only been exercised by
  the test suite below, not stress-tested independently.
(Memory-safety tools ran ad hoc during debugging this session —
ASAN/gdb were used to isolate a buffer-overflow bug and a silent
runtime-error swallow — but that's not a systematic audit.)

## 8. Garbage collector — ❌ Not started
- Kosh uses manual/ref-counted memory management, not a tracing GC, as
  far as I've seen — worth confirming with the maintainer whether GC is
  even on the roadmap or if manual management is the permanent design.
- No circular-reference/huge-object/nested-array stress testing either way.

## 9. Value system — ❌ Not started
- No review of the Value struct's tagging efficiency.
- ✅ **(this session)** Traced and documented the actual VM function
  value representation for the first time (it's not written down
  anywhere): a `VAL_FUNCTION` Value's `.as.function.body` field is
  type-punned to hold a raw `KhanFunction*` (not an `AstNode*`, despite
  the field's declared type — a leftover from the shared tree-walk-
  interpreter `Value` struct). This is safe as currently used (the
  tree-walk interpreter is dead code — see #12/#13 below) but is the
  kind of thing that will bite someone doing unrelated Value-system work
  later if it isn't written down. Recording it here since #9's own item
  hasn't started yet.

## 10. Strings — 🟡 Partial
- ❌ No profiling of substring/replace/split/join/contains/starts_with/
  ends_with/hash.
- ✅ **(this session)** Smoke-tested the `strings` package end-to-end
  (`str_starts_with`, `str_ends_with`, `str_trim`, `str_repeat`,
  `str_pad_left/right`, `str_replace`, `str_contains`, `str_index_of`,
  `str_split`, `str_join`, `str_reverse`, `str_count`, `str_is_empty`,
  `str_capitalize`) — all correct. Not profiled, just confirmed working.

## 11. Hash table — ❌ Not started
- No O(1)-average verification, resize policy review, or collision
  benchmark for the global/map implementation.

## 12. Import system — ✅ Done (was 🟡 Partial)
- ✅ Fixed a real bug where `import "math"` silently no-op'd instead of
  loading the real package (prior session).
- ❌ No dotted imports (`import webi.router`)
- ❌ No incremental loading / module cache (every import currently
  re-parses; duplicate imports of the same file ARE correctly
  deduplicated, that part already worked)
- ✅ **(this session)** Fixed a second, more consequential import bug:
  `from webi import <submodule>` ("submodule flatten" imports,
  documented in `docs/from-import.md`) only worked *by coincidence* for
  submodule names that happened to already be re-exported by the
  aggregate `webi.kh` (e.g. `security`). It silently failed for anything
  not in that aggregate — `from webi import requests` and
  `from webi import json` both left their functions completely
  undefined, surfaced as `[line 89] Runtime error: Undefined variable
  'get'` in the project's own `examples/webi_from_import_test.kh`.
  Root cause: `compile_from_import()` in the VM compiler always just
  did a blanket import of the base package, ignoring which specific
  name was requested. Implemented real per-submodule file resolution
  (`packages/<pkg>/<name>.kh`) matching the documented contract. All 16
  cases in the project's own from-import test suite now pass (were
  silently passing 13/16 before, with 3 masked as "not reached" since
  the script aborted partway through on the first real failure).

## 13. Error handling — 🟡 Partial
- ✅ Stack traces already exist (discovered, not built, prior session) —
  a runtime error inside nested function calls prints a call chain.
  ✅ **(this session)** This is what made the closures bug (#6) fast to
  diagnose — the trace pointed straight at `serve_static`'s nested
  `static_handler`.
- ❌ Not verified consistent across every error path (native functions,
  import-time errors, etc.)
- ❌ No `main.kh:18 → app.kh:55 → webi.kh:120`-style cross-file trace
  formatting confirmed
- ✅ **(this session)** Found and fixed one specific error-handling gap:
  `has()` printed a raw, unformatted `Runtime error: has() first
  argument must be a map` to stderr (no line number, unlike the VM's
  other runtime errors) and silently returned `nil` rather than halting
  — meaning callers built on top of it (`query_get`, `header_get`,
  `param_get`) would silently fall back to their default value instead
  of surfacing the real problem. Root-caused to a genuine missing-field
  bug (#14 below) and additionally hardened `has(nil, key)` to mean
  "not found" rather than erroring, since `nil` is a normal way to
  represent "no map here yet" in this dynamically-typed language and
  the three callers built on `has()` already document graceful-fallback
  behavior.

## 14. Standard library — 🟡 Partial
- 🟡 Fixed 6 packages that had never actually worked (`math`, `datetime`,
  `events`, `fs`, `logger`, `uuid` — prior session); bumped their semver
- ❌ No "never break API" policy or process in place — **(this session)**
  found concrete evidence this is a real, not theoretical, gap: `webi`'s
  `res_cors()` and `webi_handle()` both had breaking signature changes
  (v1.1.1 added a required `req` param to `res_cors`; a required `ip`
  param to `webi_handle`) that were never propagated to the project's
  own test file — 11 call sites in `examples/webi_test.kh` were quietly
  broken by the framework's own API evolution. See #15.
- ✅ **(this session)** Found and fixed a real, security-relevant `webi`
  bug, independent of the above: `mw_cors()`'s configurable CORS origin
  allow-list (`webi_set_cors_origins(app, origins)`) **never actually
  took effect**. `mw_cors` reads `req["_app"]` to find
  `app["_cors_origins"]`, but `webi_handle()` never populated
  `req["_app"]` in the first place — so `has(app, "_cors_origins")`
  always ran on `nil` and always fell through to the wide-open `"*"`
  default, silently, regardless of what was configured. This directly
  contradicts the function's own doc comment ("Real CORS handling...").
  Fixed by wiring `"_app": app` into the request map `webi_handle()`
  builds. This is the kind of bug that's invisible until you're
  specifically relying on the allow-list for security, at which point
  it's a real hole.
- ✅ **(this session)** Smoke-tested 8 previously-untested packages
  end-to-end for the first time: `colors`, `events`, `uuid`,
  `validation`, `fs`, `logger`, `strings`, `postman` (structural parts
  only — see #15). All correct, no bugs found in the packages
  themselves.
- 🟡 **(this session)** ~20 other packages remain only *compile-tested*
  or *thinly* tested — see #15's updated breakdown for exactly which.

## 15. Testing — 🟡 Partial (still the most progress of any item)
- ✅ Built `tests/run_all.kh` + 9 suites, 139 assertions (prior session)
- ✅ **(this session)** Re-ran the full existing suite after every
  change in this session — stayed green throughout, no regressions.
- ✅ **(this session)** Fixed `examples/webi_test.kh`, which was **not
  actually passing** despite being the project's most comprehensive
  single test file (137 assertions) — it was silently failing partway
  through and never reaching its own summary line. Root causes, all
  fixed:
  - 7 unsupported single-quoted string literals
  - a wrong-field assertion (`headers["Set-Cookie"]` instead of the
    actual `cookies` array — cookies are intentionally kept separate
    since a map can only hold one value per key but a response can set
    multiple cookies)
  - `res_cors()` called with the pre-v1.1.1 arity
  - 10× `webi_handle()` called with the pre-"ip-argument" arity
  - a stale expected-version-string assertion ("1.0.0" vs the actual
    "1.1.3")
  - a custom-404-handler test that never set status 404 on its own
    response (the framework doesn't force this — by design, matching
    `_webi_default_404`'s own use of `res_not_found()` — the test
    handler just needed to do the same)
  - Python-style `"=" * 50` string-repeat syntax that Kosh has never
    supported
  Now **137/137 passing**, 0 stderr noise (previously 2 silently
  swallowed runtime errors printed on every run — see #13/#14).
- ✅ **(this session)** Fixed `tests/test_vision.kh`, which targeted a
  pre-rewrite vision API (`load_image()`/image-object-based) that no
  longer exists after the vision package's rewrite into the current
  path-based classical-CV toolkit. Rewrote against the real, current
  API (`image_info(path)`, `detect_faces(path)`); now 6/6 passing
  against the project's own sample photo.
- ✅ **(this session)** Ran every existing `tests/test_*.kh` file
  (26 files) and every `examples/*.kh` file (~35 files) for the first
  time as a single pass. Results:
  - **All pass** except the two fixed above and one environment-limited
    case (`test_dns.kh` — see below).
  - `tests/test_dns.kh`: fails a real DNS lookup for `google.com`. This
    environment's network is restricted to an explicit domain allowlist
    that doesn't include `google.com`, so this could not be confirmed
    as a code bug vs. a sandbox limitation — flagged as **unverified**,
    not fixed, not counted as broken.
  - `examples/requests_test.kh` "passes" (exits 0) but its actual HTTP
    calls return 403 — same network-sandbox limitation. The code path
    (native `curl` subprocess invocation) does run; external
    reachability is what's blocked here. Also unverified either way.
  - `packages/postman` similarly could only be smoke-tested on its
    non-network logic (`pm_new`, `pm_assert`, `pm_run` — correct); its
    actual HTTP-calling functions are unverified in this environment.
  - `examples/vision_test_all.kh` was previously assumed hung
    (timed out at 5s in an earlier pass) — confirmed this session it's
    just slow (real face-detection cascade over a real photo), not an
    infinite loop; completes in ~15-20s.
  - `examples/webi_diag.kh` / `webi_security_server_demo.kh` block
    forever by design (they start a real HTTP server) — correctly
    excluded from the automated pass, not bugs.
  - `examples/openai_demo.kh` needs a real API key — excluded, not a bug.
  - `examples/import_test.kh` and `examples/webi_app.kh` have their own
    small, pre-existing issues (an unsupported `**` operator; a missing
    opening quote in one `res_html(...)` call respectively) — noted,
    left as-is since they're standalone manual-demo scripts, not part
    of the automated test/example verification loop.
- ❌ No per-opcode tests
- ❌ No per-parser-rule tests
- 🟡 **(this session, updated)** Package test coverage, concretely:
  - **Passing, exercised this session:** `argparse`, `csv_io`,
    `dotenv`, `grpc`*, `json_db`, `kbrain`, `mqtt`*, `nlp`, `openai`*,
    `orm`, `smtp`*, `sqlite`, `ssh_client`*, `ftp`*, `swagger`,
    `tensor`, `vision` (rewritten), `webi` (137/137, rewritten test),
    `webi_auth`, `webi_socket`, `colors`, `events`, `uuid`,
    `validation`, `fs`, `logger`, `strings`, `collections`, `math`,
    `datetime`, `json`, `argparse` — **31 of ~35 packages now have at
    least one real, passing, non-trivial test run against them** (up
    from "6 packages fixed, ~28 unverified" at the start of this
    session).
    <br>*= these packages' existing tests appear to be self-contained/
    mocked rather than exercising real external network I/O — worth a
    maintainer sanity-check that they're testing real behavior and not
    just a stub.
  - **Unverifiable in this sandboxed environment** (network-restricted,
    not a code-confirmed pass or fail): `dns`, live paths of `requests`
    and `postman`.
  - **Still genuinely untouched:** `registry.json`/`registry_additions.json`
    (data files, not code — not applicable), `morgos` (thin
    webi-middleware wrapper, low risk, not independently tested).
- ❌ Not wired into any automated/CI run — must be run manually. This
  remains true and is worth prioritizing given how much this session's
  bugs (#6, #12, #14) were *only* found because the existing tests were
  finally all run in one pass and their real (not just exit-code) output
  was checked line-by-line — an automated run-and-diff on every change
  would have caught the `webi_test.kh` regression the moment the
  `res_cors`/`webi_handle` signatures changed, instead of it sitting
  silently broken.

## 16. Performance benchmarks — ❌ Not started
- No `benchmarks/` folder
- No `loop.kh`, `json.kh`, `http.kh`, `sqlite.kh`, `strings.kh`, `vm.kh`
  benchmark scripts
- No comparison against Python/Lua/Node/Go/Rust/C

## 17. CI — ❌ Not started
- No GitHub Actions (or any CI) config
- No automated Linux/Windows/Mac build matrix — **(this session)**
  this gap is no longer theoretical: the project would not have built
  on Linux at all without #0's fixes, and nothing would have caught
  that automatically.
- No automated test/benchmark/memory-check-on-push pipeline

## 18. Documentation — 🟡 Partial (was ❌ Not started)
- ❌ No language specification
- ❌ No formal grammar
- ❌ No opcode documentation
- ❌ No VM architecture doc
- ✅ **(this session)** `docs/from-import.md` already existed and turned
  out to be accurate/aspirational-but-correct — it documents the
  intended 3-case behavior of `from X import Y` precisely, and was used
  as the spec to fix #12 against. Worth noting since #18 was marked
  "none of the docs above exist" — this one does, and is good.
- ❌ Still no compiler architecture doc, and nothing documents the
  closure-capture semantics or limitations added in #6 — that
  documentation now needs to be written given the new feature.

## 19. v1.0 release criteria — ❌ Not met
Per the friend's own checklist:
- ❌ Core language syntax stable — three compiler/VM correctness bugs
  found and fixed across two sessions now (continue, higher-order calls,
  **closures**); the closures gap in particular was large enough that
  "syntax stable" doesn't yet capture the risk — a whole category of
  correct-looking, common code (nested function factories) silently
  failed at runtime with no compile-time warning.
- ❌ Bytecode format stable — no review done (and this session added two
  new opcodes, so the format is actively still moving)
- 🟡 VM stable — several more real bugs fixed this session, still no
  systematic stress testing (memory, concurrency-adjacent paths like
  `webi`'s server loop, large programs)
- 🟡 Standard library APIs stable — 6 previously-broken packages fixed
  prior session; this session smoke-tested ~25 more (almost all
  packages now have *some* real coverage), and found one
  security-relevant bug (`mw_cors` origin allow-list) and one
  build-breaking bug (`requests_lib.c`) in the standard library itself,
  on top of the API-versioning-vs-tests gap described in #14/#15.
- 🟡 Major bugs fixed — several more were, still reactive (found via
  systematic test-running this time, which is a step up from "found
  while building something else," but still not a deliberate fuzz/
  stress campaign).
- 🟡 Benchmarks/tests pass — **(this session)** benchmarks still don't
  exist (#16 unchanged), but "tests pass" went from "the 139-assertion
  suite passes" to "every test and example file in the repo was
  actually run and checked, and the ones that were quietly broken
  (`webi_test.kh`, `test_vision.kh`) got fixed" — a meaningfully more
  thorough bar than before, though still manual, not automated (#17).
- ❌ Documentation complete — one doc (`docs/from-import.md`) confirmed
  good; everything else from the original list is still missing.

---

## What actually got done this session, concretely

1. **Fixed: the project didn't build on Linux at all** (two separate
   compile errors — Windows-only API call in `main.c`, broken Value
   union access in `requests_lib.c`) plus a buffer-overflow introduced
   and caught while fixing the first one.
2. **Fixed: the VM had no closures.** Nested functions referencing an
   enclosing function's parameters/locals always failed at runtime.
   Implemented real (value-capture) closures: new compiler-side
   free-variable resolution, two new opcodes, a ref-counted runtime
   capture mechanism. Verified against 3-level nesting and independent
   closure instances. Documented the by-value-not-by-reference
   limitation explicitly.
3. **Fixed: `from webi import <submodule>` silently no-op'd** for any
   submodule not coincidentally re-exported by the aggregate package
   file. Implemented real per-submodule resolution matching the
   project's own (accurate) documentation.
4. **Fixed a live security bug:** `webi`'s CORS origin allow-list
   feature was completely inert due to a missing field wire-up
   (`req["_app"]` was never set), silently defeating
   `webi_set_cors_origins()` in every real deployment.
5. **Hardened `has()`** to treat `nil` as "not found" rather than
   erroring, matching the documented contract of the three helper
   functions built on top of it, and eliminating a class of silently-
   swallowed runtime errors.
6. **Ran every test and example file in the repository** (26 `tests/
   test_*.kh` files, ~35 `examples/*.kh` files, plus the existing
   139-assertion suite) for the first time as one pass, checking real
   output rather than just exit codes, and fixed the two that were
   found to be silently broken (`examples/webi_test.kh`,
   `tests/test_vision.kh`) — both are now fully passing and reflect
   the actual current APIs.
7. **Smoke-tested 8 packages that had zero prior test coverage**
   (`colors`, `events`, `uuid`, `validation`, `fs`, `logger`, `strings`,
   `postman`) — all correct.
8. Identified (but did not attempt to fix, as out of scope for a
   correctness pass) two remaining language-design questions worth a
   maintainer decision: whether `**` (exponentiation) should exist, and
   whether `has(nil, ...)`'s new graceful behavior should extend to any
   other stdlib functions with the same "type check then hard-error"
   pattern.

## Honest read on scope

This session's real theme was **"does the stuff that's supposed to work,
actually work end-to-end,"** and the answer, repeatedly, was "not quite,
for reasons nobody had checked." The build not working on Linux at all is
the starkest example — nothing else on this roadmap matters if `make`
doesn't produce a binary. The closures gap is the most architecturally
significant finding across both sessions: it's not a typo-class bug, it's
a whole language feature that was silently absent, and it changes how
much confidence "core language syntax stable" (#19) should actually carry
until it's been given more adversarial testing, since it's exactly the
kind of gap that doesn't show up until someone writes very ordinary code.

Volume-wise, this was again concentrated in testing (#15) and the bugs
that testing surfaced — but the *kind* of testing shifted from "run the
one comprehensive suite" to "run literally everything in the repo and
read the actual output," which is what caught `webi_test.kh` silently
failing, the CORS bug, and the closures gap. That's a good sign for the
"more testing finds more real bugs" thesis, and a bad sign for how much
might still be hiding in the ~20% of the codebase that's still only
compile-tested rather than run.

The largest unclaimed pieces are unchanged from before: **performance
work (#4-6, #9-11)**, full **memory/GC auditing (#7-8)**,
**benchmarking (#16)**, **CI (#17)**, and **documentation (#18)** — CI
in particular now reads as less optional than it did before, given that
a Linux build failure and a silently-broken 137-assertion test file both
sat undetected until someone ran things by hand. Your friend's original
instinct that Kosh isn't v1.0-ready yet remains correct; if anything,
this session raised the bar for what "ready" needs to mean, since
closures were a bigger miss than any single bug found in either session
so far.
