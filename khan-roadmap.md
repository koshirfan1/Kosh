# Kosh Roadmap — Technical + Positioning (Combined Plan)

Two tracks running in parallel: fixing the real gaps that block adoption,
and building the story/visibility so people actually see it. Neither
works alone — shipping fixes with no audience gets ignored; hyping a
language with known O(n²) string concat gets torn apart on Hacker News.

---

## Track A — Technical (fix the real gaps)

Ordered by **impact on "can someone actually use this."**

### Phase 1 — Foundational correctness (do first, blocks everything else)
1. **Hash table for `{}` maps** — currently O(n) linear-scan array.
   This is the single highest-impact fix: almost any real Kosh program
   uses maps, and this is the thing that will make Kosh feel "broken"
   to a new user the moment their data grows past a few dozen keys.
   - Implement open-addressing or chaining hash table in `value.c`/new
     `hashtable.c`, wire into VM map ops.
   - Benchmark before/after with `benchmarks/map_scale.kh`.
2. **String concatenation O(n²) fix** — redundant `strlen()` per append
   in a loop. Cache length or move to a rope/builder pattern for `+=`
   in loops, or at minimum stop the repeated strlen.
   - Re-run `benchmarks/string_concat.kh` after.

### Phase 2 — Safety/robustness (needed before anyone trusts it with real programs)
3. **`try`/`catch`** — currently absent entirely. Define error-value
   propagation or exception unwinding in the VM; needs opcode(s) and
   compiler support for `try`/`catch`/`finally` blocks.
4. **GC or a documented mitigation** — full GC is a big lift. Cheaper
   interim step: detect + document/warn on common circular-reference
   patterns, or provide a `weakref`-style escape hatch, while a real
   mark-and-sweep or cycle collector is designed separately.

### Phase 3 — Performance (only after correctness, so numbers are meaningful)
5. **Dispatch loop**: switch → computed goto in `vm.c`. This is flagged
   in your own `benchmarks/RESULTS.md` as the most likely lever on the
   `loop` benchmark (194x gap vs. C).
6. Re-run all 5 benchmarks after each of the above, update
   `benchmarks/RESULTS.md` with new numbers — this file is also a
   content asset (see Track B).

### Phase 4 — Process/infra (roadmap item #17, currently ❌)
7. **CI build matrix** (Linux/macOS/Windows) — you already had a
   "didn't compile on Linux" bug ship silently; CI is what prevents
   that class of bug going forward. This is also a credibility signal
   for anyone evaluating the repo.

**Not in this plan yet, deliberately parked:** full precise GC,
Unicode/UTF-8 lexer support, v1.0 release criteria as a whole — these
are real but lower-urgency than the four items above. Revisit after
Phase 1–2 land.

---

## Track B — Positioning & Content (get it seen, honestly)

Core principle from the README's own voice: **don't round up.** Every
piece of content should be a specific, verifiable technical claim, not
a general "Kosh is fast/great" pitch — that already reads as more
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
  Kosh actually wins (2.4x). Narrow, verifiable, good technical thread.
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
| 1 | Hash table for maps | README polish + host playground |
| 2 | String concat fix | Post: face-detection mock→real story |
| 3 | try/catch | Post: JSON benchmark win |
| 4 | Dispatch loop perf | Show HN (after map fix is live) |
| 5 | CI matrix | Post: VM/bytecode deep-dive |

Start with Step 1 on both sides — it's the highest-leverage fix
(unblocks real programs) and the highest-leverage story (an already-
written, ready-to-tell anecdote).
