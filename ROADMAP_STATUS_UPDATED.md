# Khan v1.0 Roadmap — Status (Updated)

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
`khan` binary on Linux from a clean checkout.** Everything below assumes
this is now fixed and verified (`make clean && make` succeeds, `khan` runs).

---

## 1. Lexer — 🟡 Partial
- ❌ Unicode support
- ❌ UTF-8 validation
- ❌ Better indentation handling
- ❌ Escape sequence audit
- ✅ **(session 5)** Error location — column numbers now exist, and the
  exact `SyntaxError / File / Line / Column` structured format asked
  for is implemented. `Token` gained a `column` field; the lexer tracks
  a `line_start` pointer (updated at all 6 separate newline-handling
  sites — inline whitespace, both escaped/unescaped newlines inside
  string literals, `\r`/`\r\n`/`\n` blank-line handling, and the main
  `TOKEN_NEWLINE` case) rather than incrementally counting columns
  through each site separately, which would've been easy to get subtly
  wrong in one of the six spots. Verified correct across edge cases:
  after a comment line, after a multi-line string spanning several
  lines, and after a bracket-depth-spanning multi-line array literal —
  all report the exact correct column of the actual error. `Parser`
  also gained filename tracking (threaded through from `main.c` and
  both of `compiler.c`'s import-parsing call sites, plus the legacy
  tree-walk interpreter's own import path in `interpreter.c`), so
  errors now show which file they're actually in — this matters a lot
  once a script imports several packages, since previously every error
  just said `[line N]` with no indication of which file's line N.
  Error output now looks like:
  ```
  SyntaxError: Unexpected character.
    File:   examples/broken.kh
    Line:   4
    Column: 9
  ```
  replacing the old, somewhat broken two-line format (`error_at_current`
  had a dead-code placeholder that meant the first line never actually
  showed the token text at all — only a second, separate "token:" line
  did). Control characters in the "Near:" field (e.g. a bare newline
  token) are now escaped (`\n`) instead of printing a literal line
  break that looked like a formatting bug.

## 2. Parser — ✅ Done (was 🟡 Partial)
- ✅ Confirmed the parser does NOT segfault on invalid syntax in every
  case tested in session 3 (5 packages with real syntax errors all
  failed with a clean parse error, not a crash).
- ✅ **(session 5)** Systematic fuzz/negative-case testing now exists —
  `docs/parser-robustness-audit.md` has the full writeup.
  `tests/fuzz/negative_cases.py`: 38 curated genuinely-invalid programs
  (all fail cleanly, exit 65) + 9 extreme-but-valid robustness cases
  (500-deep nesting, 5000-term expressions, empty files — none crash).
  `tests/fuzz/random_fuzz.py`: mutation-based fuzzing of the project's
  own 104 real `.kh` files, run at increasing scale during this audit
  (500 → 2000 → 3000 iterations, different seeds) — **5,500 total
  mutations, zero genuine crashes or hangs.** Two false-positive
  categories investigated and correctly excluded/classified rather
  than papered over (a seed file that intentionally starts a real
  server; a mutation that creates a legitimate infinite loop in the
  mutated program's own logic — distinguished from a real stuck hang
  by checking for active ongoing output). Both scripts now run on
  every CI push (~1.5s combined).
- ✅ **(session 5)** Error messages are now consistent — see #1's
  entry above. Every parser/lexer error goes through one shared
  `report_syntax_error()` with the same `SyntaxError / File / Line /
  Column` structure.
- ✅ **(session 5)** Three parser leniencies investigated individually
  rather than assumed to be bugs (full detail in the audit doc):
  top-level `return` (intentional), `fn foo:` without parens (works,
  a language-design question not a robustness bug), and dedenting to
  an indentation level never pushed onto the indent stack (lenient
  compared to Python-style strictness, flagged for the roadmap's
  separate future indentation-handling item, not fixed here).
- 🟡 Confirmed in session 3: no `**` (exponentiation) or `*`
  string-repeat operator — only `+ - * / %` (all numeric-only, `+`
  also handles string concat). One stray example
  (`examples/import_test.kh`) assumed `**` worked; left as-is (not
  fixed) since adding an operator is a language decision, not a bug
  fix — flagging for the maintainer rather than making the call
  unilaterally.

## 3. AST — ✅ Done (was ❌ Not started)
- ✅ **(session 7)** Full node-independence audit — `docs/ast-audit.md`.
  Confirmed good: `ast_free()`'s switch is exhaustive over all 29 node
  types (verified via `-Wswitch`, already enabled, never warning); every
  string-taking AST constructor (9 of them, checked individually)
  `strdup()`s its input rather than aliasing a token's pointer into the
  transient source buffer; the node union is `memset()` to zero before
  any constructor runs, so a hypothetical future constructor that
  forgets a field defaults safely to `NULL`/`0`.
- ✅ **(session 7)** Found and fixed a serious **correctness** bug along
  the way, not just a node-independence question: array/map literals
  with more than 255 elements silently produced a **wrong length** —
  `OP_MAKE_ARRAY`/`OP_MAKE_MAP`'s element-count operand was a single
  byte with no wide variant (unlike `OP_CONST`/`OP_GET_GLOBAL`/
  `OP_DEF_GLOBAL`, which all already have one). A 5,000-element array
  literal reported length 136 (5000 mod 256); a 10,000-element one
  reported 16. No error, no crash — just silently wrong, with the
  "missing" elements still sitting in memory but permanently
  unreachable via normal indexing. Fixed by adding
  `OP_MAKE_ARRAY_WIDE`/`OP_MAKE_MAP_WIDE`, matching the existing
  `_WIDE` convention exactly. Verified at the exact 255/256 boundary
  and at 5,000/10,000-element scale; also verified this correctly
  interacts with the stack-overflow protection from session 6 (a
  literal too large to fit on the value stack now safely errors rather
  than silently truncating or crashing).
- ✅ **(session 7)** Found and fixed a real **performance** bug in the
  same audit: `ast_list_append()` — used for every statement list,
  argument list, and literal's element list — walked the entire list
  from the head on every single append, making building an N-element
  list O(n²). Verified empirically (doubling input size showed ~4x
  time, the quadratic signature) and fixed with a standard tail-cache
  pointer (low-risk, unlike the string/map quadratic-growth findings
  from earlier sessions, which needed larger Value-system changes this
  one didn't). Verified fixed: the same test that showed clear
  quadratic growth now shows flat sub-10ms timing at every size tested.
  Same category of bug as the string-concat and map-construction
  findings — worth noting this is now the *third* time this exact
  architectural pattern (an O(n) operation called in a loop) has shown
  up in this codebase.
- Both fixes: full regression suite stays green (139+137+16+6
  assertions, 1000+ fuzz mutations), dedicated ASAN pass clean.

## 4. Compiler optimizations — 🟡 Partial (was ❌ Not started)
- ✅ **(session 4)** Implemented real constant folding: a compile-time
  evaluator (`fold_constant_number`/`fold_constant_comparison` in
  `compiler.c`) that recursively evaluates pure-numeric expression
  trees (literals, `+ - * / %`, unary minus, parenthesized groupings,
  and now also comparisons `< <= > >= == !=`) and emits a single
  `OP_CONST`/`OP_TRUE`/`OP_FALSE` instead of the full unfolded
  bytecode. Deliberately conservative: numbers only (no string constant
  folding, to avoid any risk of diverging from `OP_ADD`'s runtime
  string-concat semantics), and division/modulo by a folded-zero
  divisor is explicitly excluded from folding so the existing runtime
  error path still fires instead of silently producing NaN/Inf at
  compile time — verified with a dedicated regression script
  (`tests/regression_mod_div_zero.kh`) that a folded-zero divisor still
  halts with a proper error, not a wrong answer.
- ✅ **(session 4)** While verifying the divide-by-zero exclusion, found
  and fixed a real, separate pre-existing bug: `OP_MOD` (modulo) had no
  zero-check at all (unlike `OP_DIV`, right next to it in `vm.c`) —
  `10 % 0` silently returned `-nan` instead of raising a runtime error.
  Fixed to match `OP_DIV`'s existing pattern; also fixed `OP_DIV` itself
  to free its operands before erroring (a minor leak on that specific
  error path, caught while making the two consistent).
- ❌ Dead code elimination — still not started
- ❌ Constant propagation (folding a variable's *known* constant value
  into later uses, as opposed to folding a literal-only expression
  tree) — still not started; a natural follow-up to the folding above,
  but a meaningfully different and larger piece of work (needs
  reaching, data-flow-style analysis, not just a recursive tree walk)
- ❌ Peephole optimization — still not started
- Two real correctness bugs were found and fixed in the compiler in
  session 2 (continue-jump patching, higher-order function calls), and
  a third, larger one (closures) — see #6.

## 5. Bytecode — 🟡 Partial
- ❌ No opcode count audit, no compactness pass.
- ✅ Added two new opcodes (`OP_GET_UPVALUE`, `OP_SET_UPVALUE`) in
  session 2 as part of the closures fix — noting again since it changes
  the baseline for whenever #5's audit actually happens. Full reference
  for all 38 opcodes (2 reserved/unused) now exists: `docs/opcodes.md`
  (session 3).

## 6. VM — 🟡 Partial
- ✅ **(session 4)** Found and fixed a real, unguarded segfault: `push()`/
  `pop()` in `vm.c` had **zero bounds checking** on the VM's fixed-size
  value stack (`VM_STACK_MAX` = 16384). A function with many
  parameters/locals recursing deeply can exceed that well before
  `frame_count` reaches its own separate limit (`VM_FRAMES_MAX` = 1024)
  — 1024 frames × ~20 locals/frame = 20,480 > 16,384. Verified
  reproducible as a raw, undiagnosed `SIGSEGV` (exit 139) before the
  fix. Now fails safely with a clear message and a clean exit (70)
  instead of corrupting adjacent memory — see `tests/
  regression_stack_overflow.kh` and the design tradeoff noted in the
  fix's own comment (a hard abort rather than a catchable Khan-level
  error, since threading a recoverable error through every `push()`/
  `pop()` call site in `run_loop` would be a much larger, riskier
  change than the safety win justifies here). Ordinary deep recursion
  through a function with *few* locals still hits the pre-existing,
  separate `frame_count` check first and produces the normal, already-
  working catchable "Stack overflow" runtime error — that path was
  never broken and isn't what this fix is for.
- ✅ **(this session)** Computed-goto vs switch-dispatch benchmark —
  done, and reverted. All 44 opcodes converted to computed-goto
  dispatch, benchmarked against the existing switch on `loop.kh` and
  `fib.kh`, and **the switch was faster** (5-15% depending on
  benchmark) — not the expected win. Reverted back to switch-based
  dispatch; full measurement and root-cause discussion (GCC already
  jump-tables a dense switch at `-O2`, and these benchmarks' regular
  opcode-transition patterns are close to the best case for a modern
  branch predictor handling a single shared indirect jump) in
  `docs/dispatch-perf.md`, including three real bugs found and fixed
  while converting (a nested inner switch that would have had its
  `break`s wrongly converted, five opcodes that relied on a shared
  `op` variable computed-goto doesn't have, and a bounds-check gap
  vs. corrupted/out-of-range opcode bytes) — kept documented even
  though the change itself didn't ship, so nobody re-attempts this
  exact thing without knowing it was already tried and measured.
- ✅ Stack **value** overflow/underflow detection — see session 4 entry
  just above (frame-count overflow was already checked via
  `VM_FRAMES_MAX`; the value stack itself wasn't, until now)
- ❌ No tail call optimization
- 🟡 **(session 6)** Inline-native-call optimization — investigated
  properly rather than left untouched or rushed. Measured: calling
  `len()` 3M times is ~17-22% slower than not calling anything, and
  costs about the same as calling an equivalent trivial Khan-level
  function despite very different call mechanisms — pointing at the
  shared global-hash-lookup step (not the call mechanism itself) as
  the likely larger contributor, roughly confirming the roadmap's
  original suspicion. A safe fix (generation-counter-gated
  `TableEntry*` caching per call site) is fully specified in
  `docs/call-overhead-audit.md` but not implemented — it requires
  extending the bytecode/chunk format with a per-call-site cache slot,
  a real structural change, for a win likely smaller in typical
  (non-call-heavy) code than the microbenchmark's 17-22% ceiling
  suggests. Same risk/reward judgment call as the string/map
  quadratic-growth findings: documented precisely enough to implement
  later, not rushed now.
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

## 7. Memory manager — ✅ Done (was 🟡 Partial)
- ✅ **(session 6)** The roadmap's literal ask — "10 million
  allocations. Leak? Fragmentation?" — now has a real answer, not just
  a byte-exact-but-small-scale valgrind comparison. `tests/fuzz/
  memory_stress_10m.py` runs three real scripts at true scale under
  live RSS monitoring (10M uniform allocations, 2M variable-size
  allocations specifically to stress fragmentation, 10M closure
  creations specifically re-testing session 3's fix at real scale, not
  just the 200-vs-400-iteration comparison originally used to find it).
  All three: RSS climbs briefly during allocator warmup, then stays
  **completely flat** for the rest of the run, all the way to
  completion. No leak signal, no fragmentation-growth signal, at the
  actual scale requested. Full writeup in `docs/memory-notes.md`. Runs
  in CI (informational, alongside the existing valgrind job) using the
  release build for realistic timing (10M+2M+10M iterations complete in
  ~10s combined — a debug/valgrind build would take 10-50x longer,
  impractical to run on every push).
- ✅ **(session 3)** Ran a real leak audit — not ad hoc this time. Method:
  write a loop stress-testing recursion / array-building / closure
  creation (1000 iterations each), run under `valgrind --leak-check=full`,
  then re-run at double the iteration count and diff the leak totals. A
  leak proportional to iteration count is a real unbounded-growth bug
  (matters enormously for anything long-running, like `webi`'s server
  loop); a fixed count is normal one-time startup cost. Found and fixed
  **three real, runtime-scaling leaks**, all now verified flat (zero
  growth) between 200 and 400 iterations:
  1. `OP_RETURN` discarded every local still on the stack at the point
     of return **without freeing them** — including the callee slot
     itself (a retained copy of the function/closure being called).
     This leaked on every single function call and return in the
     language, worst for closures (leaked captured values too) but
     present for any array/map/string local that outlived its natural
     scope-end by returning through it.
  2. `OP_DEF_GLOBAL` never freed the value it was replacing when
     redefining an existing global — `OP_SET_GLOBAL` already did this
     correctly, `OP_DEF_GLOBAL` didn't. This is what made the closures
     feature (#6) leak specifically: a nested `fn` compiles to
     `OP_DEF_GLOBAL` (see #9 below) and redefines the same global name
     every time its enclosing function is called.
  3. `table_free()` (process-exit teardown) freed each global's key
     string but never its value — not a growing leak, but it made
     every valgrind run report a large "indirectly lost" number
     unrelated to any real bug. Fixed for a clean baseline.

  Full writeup with call stacks and reproduction steps in
  `docs/memory-notes.md`. Verified with both valgrind (leak-check=full)
  and a separate ASAN pass (catches use-after-free/double-free, which
  leak-checking alone can't) — zero errors from either after the fix,
  full existing test suite (139 assertions + 137-assertion webi suite +
  16-case from-import suite + vision suite) still green throughout.
- ✅ **(session 3)** The remaining, *un*fixed leaks are now understood
  and documented rather than unknown: fixed one-time parser/compiler/
  native-registration bookkeeping (AST nodes, function registry
  entries, `strdup`'d native function names), never growing with
  runtime. Deliberately left as-is — see `docs/memory-notes.md` for the
  reasoning (this is normal for a short-lived script interpreter; only
  matters if Khan starts running many scripts in one long-lived
  process, which it doesn't today).
(Memory-safety tools ran ad hoc during session 2's debugging — ASAN/gdb
were used to isolate a buffer-overflow bug and a silent runtime-error
swallow. Session 3 turned this into an actual repeatable methodology,
documented above and in `docs/memory-notes.md`.)

## 8. Garbage collector — ✅ Done (was ❌ Not started)
- Khan uses manual/ref-counted memory management, not a tracing GC, as
  far as I've seen — worth confirming with the maintainer whether GC is
  even on the roadmap or if manual management is the permanent design.
- No circular-reference/huge-object/nested-array stress testing either way.
- ✅ **(session 3)** The ref-counting *mechanism itself* (for arrays,
  maps, and closures) is now verified correct under real stress via #7
  above — not just "won't obviously crash" but "frees exactly what it
  should, verified by iteration-count-independent leak totals." A
  circular-reference test (an array containing a map that references
  the array back) would still deadlock any pure-refcounting scheme
  (never reaches 0) — untested here, flagging as the next thing to
  check if this item gets picked up.
- ✅ **(this session)** Picked up exactly that flagged next step.
  Implemented a Bacon-Rajan synchronous trial-deletion cycle collector
  (the same algorithm CPython's `gc` module uses for containers),
  scoped to `VAL_ARRAY`/`VAL_MAP` — not a full tracing GC over the
  whole VM (deliberately: trial deletion only needs to walk the
  array/map object graph itself, never the VM stack/frames/globals/
  upvalues/try-handler-stack that a real tracing collector would need
  enumerated as roots). Runs automatically every 4096 array/map
  allocations, plus a `gc_collect()` native for deterministic testing.
  Full design writeup, including **two real bugs found and fixed**
  during implementation — a self-reference stack overflow (a
  reentrancy guard reused the wrong field, colliding with the
  algorithm's own white/black check) and a two-node-cycle
  use-after-free (a stale pointer left in the collector's own roots
  buffer after a cascading free) — in `docs/gc-notes.md`. Verified
  under AddressSanitizer against self-reference, two- and three-node
  cycles, map cycles, mixed array/map cycles, 500-cycle batches, a
  cycle holding a still-live external reference (confirmed the
  external object survives with a correctly decremented, not
  inflated, ref_count), and a 100-node acyclic chain (confirmed
  genuinely live data is never touched) — plus the full existing test
  suite, zero regressions. Leak measurement, not just crash-freedom:
  5,000 self-cycle iterations went from 403,548 leaked bytes without
  the collector to 3,581 bytes with it (>99% reduction), and every
  remaining byte traces to pre-existing, already-documented
  parser/AST teardown bookkeeping — nothing attributable to arrays,
  maps, or this collector. **Known limitation, documented, not fixed:**
  functions/closures aren't covered (a separate refcount in `chunk.c`'s
  `KhanClosure`) — a closure capturing a variable that indirectly holds
  the same closure would still leak. Not observed in this codebase's
  own tests/examples, flagged honestly rather than silently scoped out.

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
  ends_with/hash — the *functions* were smoke-tested (below), but not
  profiled for performance.
- ✅ **(session 3)** Smoke-tested the `strings` package end-to-end
  (`str_starts_with`, `str_ends_with`, `str_trim`, `str_repeat`,
  `str_pad_left/right`, `str_replace`, `str_contains`, `str_index_of`,
  `str_split`, `str_join`, `str_reverse`, `str_count`, `str_is_empty`,
  `str_capitalize`) — all correct. Not profiled, just confirmed working.
- ✅ **(session 4)** Found and precisely traced a real performance bug
  via `benchmarks/string_concat.kh`: `+`-concatenation (`OP_ADD`'s
  string branch, `vm.c`) calls `strlen()` on the *accumulated* string
  every single time, making repeated concatenation in a loop O(n²) —
  verified empirically (5x more iterations took ~80x longer). Root
  cause and why it wasn't fixed here (would need a length-tracking
  string representation, a real Value-system change) written up in
  `benchmarks/RESULTS.md`. This is the same category of bug as #11's
  map finding below — something O(n) per call, called in a loop.

## 11. Hash table — ✅ Done (was 🟡 Partial, was ❌ Not started)
- ✅ **(session 4)** Full audit done — `docs/hash-table-audit.md`. The
  finding is bigger than "check the collision handling": **there are
  two unrelated data structures both informally called a
  table/map, and only one is actually a hash table.**
  - `Table` (`vm.c`, used only for the VM's globals) is a real,
    correctly-implemented hash table: FNV-1a hashing, linear-probing
    collision resolution, doubles at 75% load factor. Nothing wrong
    found here.
  - **The Khan-language `{}` map type — used everywhere in real Khan
    programs — is a linear-scan array, not a hash table at all.**
    `map_set`/`map_get` in `value.c` do a full O(n) scan of every
    existing key on every call. Verified empirically: building a map
    with 4,000 keys one at a time, then 8,000, showed clearly
    super-linear (roughly quadratic) growth, not the O(n) a real hash
    table would give.
  - Not fixed here — it's a real Value-system-wide change (touches
    `interpreter.h`'s map representation and every direct reader of it
    across `value.c`, `vm.c`, `vision_lib.c`/`vision_cv.c`, `json_lib.c`
    and likely more), and there's a real design question to resolve
    first (the current linear array happens to preserve insertion
    order on iteration; a naive hash-table swap would silently lose
    that unless deliberately preserved). Fully written up with a
    recommended approach in the audit doc — this is now the single
    most concrete, well-scoped item on the whole roadmap for whoever
    picks up performance work next.
  - Added `benchmarks/map_scale.kh` (+ Python/JS equivalents) so this
    is now a trackable, re-runnable metric rather than a one-time
    finding.
  - ✅ **(fixed)** The linear scan is now replaced with a real
    open-addressing hash index (FNV-1a, linear probing) sitting
    alongside the existing insertion-ordered `entries[]` array — see
    "Resolved" section at the end of `docs/hash-table-audit.md` for
    the full writeup. `map_scale.kh` (4,000 keys): 0.031s → 0.005s.
    A 50,000-key stress case: 8.99s → 0.012s (~725x), confirming the
    win scales with the square of map size, as expected for an
    O(n²)→O(n) fix. All existing test suites pass unchanged,
    including insertion-order-dependent output.

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

## 13. Error handling — 🟡 Partial (was 🟡 Partial — try/catch now ✅)
- ✅ **(this session)** `try`/`catch`/`throw` implemented — was
  completely absent (a named weakness in the README) — now a real
  language feature: lexer/parser/AST/compiler/VM all support it,
  catches both user `throw`s and every existing built-in runtime error
  (division by zero, undefined variable, bad index, etc. — all ~13
  existing `runtime_error()` call sites are catch-aware, not just new
  user-facing throws). Handles nesting, re-throwing from inside a
  catch block, and exceptions crossing recursive function calls.
  Full design writeup, including a real bug found and fixed during
  implementation (a `continue` trapped inside a `do{}while(0)` macro
  wrapper — classic C pitfall, would have silently corrupted the VM
  stack on the *second* try/catch in any script), in
  `docs/trycatch-implementation.md`.
  - **Known limitation, documented, not yet fixed:** an exception
    thrown from inside a native-callback context (e.g. a comparator
    passed into a sort, or an `array.map()` callback) isn't catchable
    by a handler in an enclosing scope, if that native function
    re-enters the VM via `vm_call_fn()`'s separate recursive
    `run_loop()` call rather than the normal in-loop `OP_CALL` path.
    Fails safe (falls back to pre-try/catch behavior for that one
    case — the native call gets a clean nil back, program keeps
    running), just doesn't catch across that specific boundary.
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
  - Python-style `"=" * 50` string-repeat syntax that Khan has never
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

## 16. Performance benchmarks — 🟡 Partial (was ❌ Not started)
- ✅ **(session 4)** `benchmarks/` now exists: `loop.kh`, `fib.kh`,
  `string_concat.kh`, `json_bench.kh`, `map_scale.kh`, each with a
  Python and Node equivalent (and a C equivalent for `loop`/`fib`, as a
  native-floor reference point) — plus `benchmarks/run.py`, a small
  runner that times all of them and prints a comparison table, and
  `benchmarks/RESULTS.md` with real, actually-executed numbers and
  analysis (not placeholder numbers).
- 🟡 **Only Python and Node** were available to compare against in this
  environment — Lua/Go/Rust from the original ask are not installed
  here. `run.py` is written so adding another language is a small,
  repetitive addition following the existing pattern, not a rewrite.
- ✅ This is where the string-concatenation quadratic-growth finding
  (see #10) and the map linear-scan finding (see #11) actually came
  from — benchmarking wasn't just a checkbox item, it's what surfaced
  both of this session's two biggest performance findings.
- 🟡 Deliberately not rigorous (no warmup, no statistical repeats/
  variance) — explicitly documented as such in `RESULTS.md` rather than
  presented as more precise than it is.
- ❌ Not wired into CI as a tracked-over-time metric yet (the CI
  memory-check job pattern from #17 could be extended similarly, but
  wasn't done here — timing comparisons across different CI runner
  hardware are noisier than a fixed pass/fail check, so this needs more
  thought than copy-pasting the existing job pattern).

## 17. CI — 🟡 Partial (was ❌ Not started)
- ✅ **(session 3)** There was actually already a `.github/workflows/
  c-cpp.yml` — but it only ran `make` on Linux/Windows and uploaded the
  binary. It never ran a single test. This is precisely the gap that
  let a Linux build failure (#0) and a silently-broken 137-assertion
  test file (#15) both sit undetected until someone ran things by hand
  — a CI config that only checks "did it compile" would have stayed
  green through both of those. Rewrote it to actually gate on:
  - the 139-assertion core suite (`tests/run_all.kh`)
  - the 137-assertion webi suite (`examples/webi_test.kh`)
  - the from-import regression suite (`examples/webi_from_import_test.kh`)
  - the vision suite (`tests/test_vision.kh`)
  - every package importing cleanly (the same sweep used manually in
    session 2, now automated)
  — on Linux, Windows, **and now macOS** (the matrix only had two of
  the three platforms the friend's list asked for).
- ✅ **(session 3)** Added a separate `memory-check` job (Linux,
  valgrind, `continue-on-error: true` since exact leak byte-counts can
  shift across glibc versions from the expected one-time startup cost
  described in #7 — but this would still catch anything that jumps by
  a large multiple, which is what the real bugs in #7 looked like).
- ✅ **(session 4)** Added two more required checks, both verifying a
  bug stays fixed by checking for the *correct* failure mode, not just
  "did it succeed": the div/mod-by-zero constant-folding-exclusion
  regression (`tests/regression_mod_div_zero.kh`, expects clean exit 70
  for both `div` and `mod` modes) and the stack-overflow safety fix
  (`tests/regression_stack_overflow.kh`, expects exit 70, explicitly
  fails the job if it sees exit 139/segfault instead). Verified both
  shell blocks in isolation, matching how GitHub Actions actually runs
  each step as its own subshell.
- 🟡 Benchmarks still not wired in (see #16 — deliberately not done yet,
  timing noise across CI hardware needs more thought first)
- ❌ Unverified end-to-end — this was written and the underlying shell
  logic/commands were tested locally (they all pass), but the workflow
  YAML itself hasn't been run through actual GitHub Actions in this
  environment (no such access here) — flagging this explicitly rather
  than claiming more confidence than is warranted. This caveat has now
  applied across two sessions in a row; if this project has any GitHub
  Actions minutes available, actually pushing this and watching it run
  once would be higher-value than most other things left on this list.
- ✅ **(this session)** Re-verified every one of this workflow's test
  gates by hand, end to end, against a build containing this session's
  full set of changes (hash-map rewrite, string-concat partial fix,
  try/catch, the cycle collector, and the dispatch-loop experiment
  that was tried and reverted) — not just "does it still compile."
  All passed unchanged: the 158-assertion core suite (9 suites, all
  100%), the 137-assertion webi suite, the 16-check from-import suite,
  the 6-assertion vision suite, every package importing cleanly, both
  div/mod-by-zero-still-raises-an-error regressions, the stack-overflow
  clean-failure regression, the 38+9-case parser negative/robustness
  suite, and 1000 iterations of mutation fuzzing (zero findings).
  Additionally compared AddressSanitizer leak totals on the core suite
  between a completely unmodified build and this session's combined
  build: 71,180 bytes / 4,099 allocations (pristine) vs. 72,289 bytes
  / **3,986** allocations (this session) — same magnitude, fewer
  allocations, confirming none of this session's four features
  introduced a new leak. The "not run through actual GitHub Actions"
  caveat above still stands (no access to that here either) — this
  doesn't close that gap, but it's a real, substantive check beyond
  what existed before this session on top of it.

## 18. Documentation — 🟡 Partial (was 🟡 Partial, but thin)
- ❌ No language specification, no formal grammar
- ✅ `docs/from-import.md` (confirmed accurate in session 2 — documents
  the intended 3-case behavior of `from X import Y` precisely, and was
  used as the spec to fix the import-system bug it describes)
- ✅ **(session 3)** `docs/opcodes.md` — a real opcode reference for all
  38 opcodes (2 of which, `OP_BREAK`/`OP_CONTINUE`, are confirmed
  dead/unused enum entries — `break`/`continue` actually compile to
  `OP_JUMP` with patch-lists, documented as such). Covers stack effects,
  the `OP_CALL` calling convention (including the callee-slot-before-
  frame->slots subtlety that caused a real off-by-one bug previously),
  and the `OP_DEF_GLOBAL` function-registry special case that's central
  to how both regular functions and closures actually work at runtime.
- ✅ **(session 3)** `docs/memory-notes.md` — full writeup of the #7
  memory audit: what was found, why, the fix, and how to reproduce the
  methodology for future audits.
- ✅ **(session 4)** `docs/hash-table-audit.md` — full writeup of the
  #11 finding (the Khan-language map type isn't a hash table).
- ✅ **(session 4)** `benchmarks/RESULTS.md` — real, executed benchmark
  numbers plus analysis, including the string-concatenation and map
  quadratic-growth findings.
- 🟡 Still no full VM architecture doc or compiler architecture doc,
  though `opcodes.md` + `memory-notes.md` + `hash-table-audit.md` +
  `benchmarks/RESULTS.md` together now cover a meaningful slice of "how
  the VM actually works, and where it doesn't scale" that didn't exist
  at the start of session 3.
- ❌ Nothing documents the closure-capture semantics/limitations from
  #6 in a dedicated language-features doc (it's described in the
  roadmap here and in code comments, but not in end-user-facing docs).

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

## What actually got done in session 3, concretely

1. **Verified session 2's claims against real code rather than trusting
   the writeup** — closures, the from-import fix, and the 137/137 webi
   test were all independently re-run and confirmed correct before
   building anything further on top of them.
2. **Found and fixed three real, runtime-scaling memory leaks** via a
   systematic (not ad hoc) valgrind methodology: `OP_RETURN` abandoning
   locals and the callee slot without freeing them, `OP_DEF_GLOBAL`
   never freeing the value it replaces (which is what made closures
   leak specifically, since nested `fn` redefines a global on every
   call), and `table_free()` skipping value cleanup at exit. Verified
   flat (zero growth) at both 200 and 400 loop iterations, cross-checked
   with ASAN for use-after-free, zero regressions across the entire
   existing test suite.
3. **Documented the findings properly** (`docs/memory-notes.md`) rather
   than just leaving fixed code with no explanation — includes a
   reproducible methodology for the next person to run this audit
   again.
4. **Rewrote CI from "checks if it compiles" to "actually gates on the
   test suite passing"** — added the macOS leg the build matrix was
   missing, wired in all 4 major test suites plus the package-import
   sweep as required checks, and added an informational valgrind job.
5. **Wrote a real opcode reference** (`docs/opcodes.md`) covering all
   38 opcodes, the calling convention, and the `OP_DEF_GLOBAL`
   function-registry mechanism that both regular functions and closures
   depend on.
6. **Flagged, but did not fix** (correctly out of scope for a
   correctness/stability pass): nested `fn` declarations compile as
   globals rather than function-local scope. Currently safe in
   practice for every pattern actually used in this codebase, but
   correct by coincidence of usage, not by design guarantee — noted in
   `docs/memory-notes.md` as a compiler-architecture item for whoever
   picks up #3/#4.

## Honest read on scope

Session 2's theme was "does the stuff that's supposed to work, actually
work end-to-end." Session 3's theme was **"does the stuff that's
supposed to be freed, actually get freed"** — a systematic pass rather
than the ad hoc ASAN/gdb use from session 2, and it found exactly the
kind of bug that pass-through testing (does the script produce the right
output?) can never catch: all three leaks fixed here were completely
invisible to the 139+137+16+6-assertion test suites, because leaking
memory doesn't change a program's *output*, only its long-run resource
use. This is worth sitting with — a fully green test suite was
compatible with the VM leaking on every single function call. The
`webi` server (a genuinely long-running process, unlike a one-shot
script) would have slowly consumed more and more memory over its
lifetime purely from ordinary request handling, and nothing in the
existing test suite could have shown that.

The build not working on Linux at all (#0, session 2) and this session's
leak findings share a root cause worth naming directly: **this project's
only verification signal before session 2 was "does it look right when I
run it once," and that signal cannot see build-matrix failures or
resource leaks by construction.** CI (#17) and the memory audit (#7) are
both, in their own way, this session's answer to that — one automates
catching regressions going forward, the other establishes what "correct"
even means for the value lifecycle and gives a repeatable way to check
it again later.

The largest unclaimed pieces are narrower than before but still real:
**performance/compiler optimization work (#4-6, #9-11)** — nothing here
touched constant folding, dispatch benchmarking, or hash table/string
profiling — **benchmarking (#16)**, which still doesn't exist at all,
and **full documentation (#18)** — a language spec and formal grammar
are still missing, though the VM/memory side is now meaningfully
better documented than the compiler/parser side. CI (#17) went from
nonexistent-in-practice to real but unverified-on-actual-GitHub-Actions,
which is an honest caveat, not a claim of completion. Your friend's
original instinct that Khan isn't v1.0-ready yet remains correct — but
the specific *reason* it isn't ready has shifted across these three
sessions from "does it even build" to "does it work" to "does it work
without silently consuming more memory forever," which is a real
trajectory toward v1.0, not just a longer list of caveats.

---

## What actually got done in session 4, concretely

1. **Implemented real constant folding** (compile-time evaluation of
   pure-numeric expressions, including comparisons) — the first actual
   compiler optimization on the whole roadmap, deliberately excluding
   anything that could change program behavior (short-circuit and/or,
   folded-zero divisors).
2. **Found and fixed a genuine pre-existing bug while verifying the
   above**: `OP_MOD` never checked for zero, silently returning `-nan`
   instead of erroring — `OP_DIV` right next to it already did this
   correctly. Also fixed a minor leak-on-error-path inconsistency
   between the two.
3. **Found and fixed a real, unguarded segfault**: `push()`/`pop()` had
   zero bounds checking on the VM's value stack. Reproduced as a raw
   SIGSEGV via deep recursion through a many-local function (exceeds
   the value stack before the separate frame-count limit kicks in).
   Now fails safely with a clear message instead of corrupting memory.
4. **Built the benchmark suite from scratch** (#16, previously fully
   unstarted): 5 benchmarks × Khan/Python/Node/C-where-applicable, a
   runner, and real executed results with analysis — not placeholder
   numbers.
5. **The benchmarks immediately found two real, concrete performance
   bugs**, both documented in depth rather than just measured:
   - String concatenation is O(n²) in a loop — traced to a specific,
     unnecessary `strlen()` call on the accumulated string in `OP_ADD`.
   - Building a `{}` map is O(n²) — because **Khan's map type is a
     linear-scan array, not a hash table at all**, despite roadmap
     item #11 describing it as if it were. This is arguably the single
     most consequential correctness-adjacent finding of this session,
     since `{}` maps are used pervasively throughout every real Khan
     program.
6. **Did a full hash-table audit** (#11, previously fully unstarted)
   and found the above — plus confirmed the VM's *actual* hash table
   (used only for globals) is a correct, standard implementation with
   no issues.
7. **Extended CI** with two more required checks verifying the div/
   mod-zero and stack-overflow fixes stay fixed by checking for the
   *correct* failure mode specifically, not just "did something fail."
8. Deliberately did **not** attempt to fix the string or map quadratic-
   growth findings — both would require real Value-system-representation
   changes (a length-tracking string type; an actual hash table for
   maps, with an insertion-order-preservation question to resolve
   first) that deserve their own dedicated, carefully-tested pass
   rather than being rushed in alongside a benchmarking session.

## Honest read on scope, updated

Session 4's throughline: **every piece of new work this session found
at least one real bug or performance problem nobody had previously
identified** — constant folding surfaced the modulo-by-zero bug;
building a proper benchmark suite (the most "just infrastructure"-
sounding task of the four) is what actually found both quadratic-growth
bugs; even just carefully re-verifying the divide-by-zero exclusion
surfaced the stack-overflow segfault as a related but independent
concern worth checking. This continues the pattern from sessions 2-3:
**the moment anyone actually exercises a code path with real inputs
rather than assuming it works, something real turns up.** Four sessions
in, this is no longer a coincidence — it's a fair description of this
codebase's actual state, and the honest implication is that most of the
remaining ❌ items on this list (particularly #4's unfinished pieces,
dead code elimination and constant propagation; #9's Value system
tagging audit; and a real fuzz-testing pass on the parser from #2) would
likely surface more of the same if anyone sat down and actually did them,
not because this project is unusually bad, but because it had
approximately zero systematic verification before session 2 started.

The two performance findings from this session (string concat, map
scaling) are worth calling out as different in kind from everything
fixed in sessions 2-4 so far: every previous fix (closures, continue,
higher-order calls, the three memory leaks, the stack-overflow segfault)
was a **correctness** bug — wrong answer or crash. These two are
**performance** bugs — the answer is right, the program just gets
disproportionately slower as real-world usage grows, which is a
different (and in some ways more insidious) risk profile: it won't show
up in a test suite checking correctness, only in production usage that
happens to build a large-enough map or a long-enough string. That they
were found via benchmarking rather than testing is exactly the point of
having both.

