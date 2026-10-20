# Khan Roadmap — Technical + Positioning (Combined Plan)

Two tracks running in parallel: fixing the real gaps that block adoption,
and building the story/visibility so people actually see it. Neither
works alone — shipping fixes with no audience gets ignored; hyping a
language with known O(n²) string concat gets torn apart on Hacker News.

---

## Track A — Technical (fix the real gaps)

Ordered by **impact on "can someone actually use this."**

### Phase 1 — Foundational correctness (do first, blocks everything else)
1. ✅ **DONE — Hash table for `{}` maps.** Was an O(n) linear-scan
   array; now a real open-addressing hash index (FNV-1a, linear
   probing, ~70% load-factor growth) sitting alongside the existing
   insertion-ordered `entries[]` array, so every one of the 20+
   existing `map_set`/`map_get` callers and every direct
   `AS_MAP_ENTRIES` reader (JSON encoding, `keys()`, header iteration)
   needed zero changes. Measured: `map_scale.kh` (4,000 keys) 0.031s →
   0.005s; a 50,000-key stress case 8.99s → 0.012s (~725x). Full test
   suites pass unchanged. Files: `src/interpreter.h`, `src/value.c`,
   writeup in `docs/hash-table-audit.md`, status in
   `ROADMAP_STATUS_UPDATED.md` item 11.
2. 🟡 **PARTIAL — String concatenation O(n²) fix.** Removed the
   redundant `strlen()` rescan on the growing accumulator (cached
   length via a hidden header, same technique as Redis's SDS strings) —
   real, verified ~2.4x win (100k-iteration `s = s + "x"`: 2.42s →
   1.0s). **But the O(n²) is not fully gone** — traced the remaining
   cause to `OP_SET_LOCAL`/`OP_SET_GLOBAL`/`OP_SET_UPVALUE` in `vm.c`,
   which deep-copy the assigned value on every write by design (avoids
   aliasing bugs). Closing this fully means giving strings spare
   capacity *and* changing assignment to move ownership instead of
   copying when the source is a dead stack temporary — a real change
   to the VM's copy discipline, scoped but not done yet. Files:
   `src/value.c`, `src/value.h`, `src/interpreter.h`, `src/vm.c`,
   writeup in `benchmarks/RESULTS.md`'s `string_concat` section.

### Phase 2 — Safety/robustness (needed before anyone trusts it with real programs)
3. ✅ **DONE — `try`/`catch`/`throw`.** Full lexer/parser/AST/compiler/
   VM implementation, catching both user `throw`s and every existing
   built-in runtime error. Handles nesting, re-throw, and exceptions
   crossing recursive calls. A real bug was found and fixed mid-
   implementation (a `continue` trapped inside a `do{}while(0)` macro
   — see `docs/trycatch-implementation.md` for the full story, kept
   deliberately detailed since it's a C pitfall worth remembering).
   One documented known limitation: exceptions from inside a native-
   callback context (e.g. an `array.map()` callback) aren't catchable
   by an enclosing handler — fails safe, not silently. Files:
   `src/token.h`, `src/lexer.c`, `src/ast.h`, `src/ast.c`,
   `src/parser.c`, `src/chunk.h`, `src/vm.h`, `src/vm.c`,
   `src/compiler.c`; roadmap status in `ROADMAP_STATUS_UPDATED.md`
   item 13.
4. ✅ **DONE — Cycle collector for circular references.** Implemented
   a Bacon-Rajan trial-deletion collector (scoped to arrays/maps,
   not a full tracing GC — never needs to enumerate the VM's stack/
   frames/globals as roots, only walks the array/map object graph
   itself). Two real bugs found and fixed during testing (a
   reentrancy-guard field collision causing a self-reference stack
   overflow, and a stale-pointer use-after-free on two-node cycles) —
   full story in `docs/gc-notes.md`. Verified under AddressSanitizer:
   self-reference, 2- and 3-node cycles, map cycles, mixed cycles,
   500-cycle batches, live-child survival, acyclic-data safety, plus
   the full existing suite — zero regressions. Leak measurement:
   5,000-iteration cycle workload went from 403,548 leaked bytes to
   3,581 (>99% reduction), remainder all pre-existing/unrelated.
   Known, documented limitation: closures aren't covered (separate
   refcount in `chunk.c`). Files: `src/interpreter.h`, `src/value.c`,
   `src/khan_stdlib.c`/`.h`, `src/vm_libs.c`; roadmap status in
   `ROADMAP_STATUS_UPDATED.md` item 8.

### Phase 3 — Performance (only after correctness, so numbers are meaningful)
5. ✅ **DONE (negative result) — Dispatch loop.** Converted all 44
   opcodes to computed-goto (`goto *dispatch_table[...]`), benchmarked
   against the existing switch, and the switch turned out FASTER
   (5-15% depending on benchmark) — reverted. Full measurement, why
   (GCC already jump-tables a dense switch at `-O2`; these benchmarks'
   regular dispatch patterns favor a shared predictor slot), and three
   real bugs found while converting (documented so nobody re-attempts
   this blind) in `docs/dispatch-perf.md`. Dispatch mechanism is
   confirmed NOT where Khan's per-op overhead comes from — the
   `value_copy()`-per-operation cost flagged in `benchmarks/RESULTS.md`
   remains the most promising next thing to actually profile.
6. Re-run all 5 benchmarks after each of the above, update
   `benchmarks/RESULTS.md` with new numbers — this file is also a
   content asset (see Track B).

### Phase 4 — Process/infra
7. ✅ **DONE — CI build matrix.** Turns out this already existed from
   a prior session (`.github/workflows/c-cpp.yml`) — Linux/macOS/
   Windows, gating on the full test suite (not just "does it
   compile"), a valgrind memory-check job, and two regression tests
   for previously-fixed bugs (div/mod-by-zero constant-folding, stack-
   overflow clean-failure). This session re-verified every one of its
   gates by hand against a build containing all of this session's
   changes (hash map, string fix, try/catch, GC, dispatch experiment)
   — all pass unchanged, plus a leak-count comparison (pristine vs.
   this session's build, same test suite) confirming zero new leaks
   introduced by any of the four features. One gap remains and can't
   be closed from here: the workflow YAML itself has never actually
   been run through GitHub Actions (no access to verify that end-to-
   end) — flagged in `ROADMAP_STATUS_UPDATED.md` item 17 across
   multiple sessions now; pushing this once and watching it run would
   be the highest-value single action left on the whole technical list.

**Not in this plan yet, deliberately parked:** full precise GC,
Unicode/UTF-8 lexer support, v1.0 release criteria as a whole — these
are real but lower-urgency than the four items above. Revisit after
Phase 1–2 land.

---

## Track B — Positioning & Content (get it seen, honestly)

Core principle from the README's own voice: **don't round up.** Every
piece of content should be a specific, verifiable technical claim, not
a general "Khan is fast/great" pitch — that already reads as more
credible than 90% of hobby-language launches, keep it that way.

### Phase 1 — Sharpen what's already there
1. **README first-impression pass**: the "What's Real, What's a Known
   Gap" section is genuinely your strongest asset — most projects hide
   this. Make sure it's above the fold, not buried in a table of
   contents. Consider pulling the face-detection-mock-then-fix story
   into its own short section near the top — it's the single best
   trust-building anecdote in the whole repo.
2. **Pin/host the playground** — right now it's "not hosted anywhere
   yet." A live, clickable WASM playground is worth more than paragraphs
   of README for a first-time visitor. GitHub Pages is free and you
   already have the built `khan.wasm`/`khan.js`.

### Phase 2 — Content pieces (each = one specific, narrow claim)
Pick from these, each is a standalone post/thread:
- **"I faked a feature, then ripped it out and rebuilt it for real"**
  — the vision/face-detection story. Best for HN/Reddit — humility +
  technical depth is the exact combination that does well there.
- **"Why my JSON parser beats Python's"** — the one benchmark where
  Khan actually wins (2.4x). Narrow, verifiable, good technical thread.
- **"Building a bytecode VM from scratch in C — what I got wrong"**
  — a walkthrough of the dispatch loop, calling convention
  (`docs/opcodes.md`), memory model (`docs/memory-notes.md`).
- **"A real Tesseract OCR bridge in a language I wrote myself"** —
  visual, demoable, good for a short video/thread with before/after.

### Phase 3 — Where to post (in order of fit)
1. **r/ProgrammingLanguages** — audience explicitly values process and
   honesty over polish. Best first stop.
2. **Hacker News (Show HN)** — do this *after* Phase 1 fixes land
   (hash map especially) — HN commenters will benchmark it live and
   the O(n) map would get found immediately.
3. **Twitter/X threads** — short, one-claim-per-thread, link to repo.
4. **GitHub itself** — good topics/tags (`programming-language`,
   `bytecode-vm`, `compiler`), pin the playground link in repo
   description.

### Phase 4 — Ongoing cadence
- One technical post every 1-2 weeks, each tied to a real, shipped
  change (ties Track A and Track B together — never post about
  something not actually in the repo yet).
- Update `benchmarks/RESULTS.md` and `ROADMAP_STATUS_UPDATED.md`
  publicly after each session — the "honest status, not optimistic
  status" framing is itself a recurring content hook.

---

## Suggested sequencing (both tracks together)

| Step | Track A | Track B |
|---|---|---|
| 1 | ✅ Hash table for maps (done) | README polish + host playground |
| 1b | 🟡 String concat partial fix (~2.4x, root cause still open) | Post: face-detection mock→real story |
| 3 | ✅ try/catch (done) | Post: JSON benchmark win |
| 3b | ✅ GC / cycle collector (done) | — |
| 4 | ✅ Dispatch loop — tried, measured slower, reverted | Show HN (after map fix is live) |
| 5 | ✅ CI matrix — already existed, re-verified this session | Post: VM/bytecode deep-dive |

Start with Step 1 on both sides — it's the highest-leverage fix
(unblocks real programs) and the highest-leverage story (an already-
written, ready-to-tell anecdote).
