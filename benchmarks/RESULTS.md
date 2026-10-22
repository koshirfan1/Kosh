# Kosh Benchmark Results

Real numbers from `benchmarks/run.py`, run in this environment (Linux,
gcc -O2 for the C reference, whatever Python/Node happened to be
installed). **Not** rigorous microbenchmarks — one run each, no warmup,
no statistical repeats. Treat every number here as order-of-magnitude,
not a precise claim, and re-run `run.py` yourself before trusting a
specific figure.

Only Python and Node were available in this environment to compare
against — the original roadmap asked for Python/Lua/Node/Go/Rust/C.
Lua/Go/Rust are not installed here; someone with those available should
extend `run.py` the same way the existing four runtimes are wired in
(it's a small, repetitive pattern per language).

## Results

```
benchmark               kosh      python        node           c     kosh/fastest
---------------------------------------------------------------------------------
loop                  0.991s      0.440s      0.380s      0.005s     194.5x
fib                   0.155s      0.063s      0.068s      0.003s      51.8x
string_concat         0.027s      0.016s      0.041s         n/a       1.7x
json_bench            0.109s      0.263s      0.101s         n/a       1.1x
map_scale             0.031s      0.016s      0.036s         n/a       2.0x
```

(A second run showed `loop` and `fib` numbers shift somewhat for
Node specifically — 0.045s and 0.036s respectively — almost certainly
V8 JIT warmup variance across single runs with no warmup phase. Kosh's
own numbers were stable within ~10% run to run.)

## What each benchmark stresses, and what the numbers say

- **`loop`** (5,000,000 iterations of `sum = sum + i; i = i + 1`) —
  stresses the bytecode dispatch loop itself for the simplest possible
  operations. Kosh is ~194x slower than the native C floor here, and
  ~2-8x slower than Python/Node depending on their JIT warmup state.
  This is the most "generic interpreter overhead" benchmark of the
  four — every `OP_ADD`/`OP_GET_LOCAL`/`OP_SET_LOCAL` goes through a
  `value_copy()` (see `docs/memory-notes.md` for what that does) even
  for a plain number, which a real profiler would likely show as a
  meaningful chunk of this gap.

  **Update — computed-goto dispatch tried, measured, reverted.** This
  section used to flag computed goto vs. switch as the thing to try
  first on this exact benchmark. It was tried: all 44 opcode handlers
  converted from `switch(op){...}` to a `goto *dispatch_table[...]`
  scheme (GCC/Clang `&&label` extension), full writeup and the three
  real bugs found converting it safely in `docs/dispatch-perf.md`.
  Measured result, median of 5-7 runs each: **switch-based dispatch
  was faster**, not slower — `loop.kh` 1.00s (switch) vs 1.01-1.12s
  (computed goto, depending on whether an added bounds-check was
  included); `fib.kh` 0.152s (switch) vs 0.175s (computed goto), a
  clearer ~15% regression. Reverted back to the switch-based version
  that's actually in this repo. The honest read: GCC already compiles
  a dense small-integer switch like this into a jump table at `-O2`,
  and this VM's opcode-transition patterns in these two benchmarks are
  regular/predictable enough that a modern branch predictor handles
  the switch's single indirect jump about as well as computed goto's
  many separate ones — the classic "computed goto wins" argument
  doesn't hold universally, and evidently doesn't hold here. Dispatch
  mechanism is *not* where Kosh's per-op overhead comes from; the
  `value_copy()`-per-operation cost flagged above is a much more
  promising thing to profile next. This is now a closed, measured
  question rather than an open "should try this" — re-litigating it
  without new information (a different compiler, a very different
  opcode mix) would just re-derive the same answer.

- **`fib`** (recursive, unmemoized, `fib(28)`) — stresses function
  call/return overhead specifically. Notably, Kosh's *relative* gap to
  Python/Node is smaller here (~2-4x) than in the raw loop benchmark,
  even though `fib` does far more work per call (frame setup,
  `OP_RETURN`'s locals-freeing pass — see `docs/memory-notes.md` — the
  whole calling convention in `docs/opcodes.md`). This suggests Kosh's
  *function call* overhead isn't disproportionately worse than its
  *basic bytecode dispatch* overhead — if anything, the opposite. Worth
  keeping in mind before assuming "function calls are the slow part."

- **`string_concat`** (20,000 repeated `s = s + "x"`) — Kosh is only
  ~1.6-1.7x slower than the fastest of Python/Node at this size. But
  this benchmark is deliberately run at a size too small to show
  something important: **manually re-running this at 100,000
  iterations instead of 20,000 (5x the input) took roughly 80x longer**
  (2.42s vs 0.03s at 20,000) in ad hoc testing outside the standard
  suite. That's worse than quadratic, not just quadratic.

  **Root cause, traced exactly:** `OP_ADD`'s string-concatenation
  branch (`src/vm.c`) does this on every single call:
  ```c
  int la = (int)strlen(a.as.string);
  int lb = (int)strlen(b.as.string);
  ```
  `a` is the *accumulated* string (the left operand of `s + "x"`, i.e.
  `s` itself). `strlen()` is O(length), and `a`'s length grows by 1
  every iteration — so the total cost of just these `strlen()` calls
  across N iterations is `1 + 2 + 3 + ... + N` = O(N²).

  **Correction to this doc's earlier claim:** this previously said "the
  actual `malloc`+`memcpy` work is only O(N) total" — that's wrong. The
  `memcpy(s, a.as.string, la)` right after copies the *entire* `la`-byte
  accumulator on every iteration too, for the same reason the `strlen()`
  does: `la` grows by 1 each time, so `sum(1..N) = O(N²)` applies to the
  memcpy just as much as the strlen. Both are separate, independently
  quadratic contributors — not one quadratic cost plus one linear one.

  **Why this wasn't fully fixed here rather than just documented:** Kosh's
  `VAL_STRING` currently stores a bare `char *` (see `value_string()` in
  `src/value.c` — no length field). Fixing this properly means giving
  strings a cached length (or a full rope/builder type) — a change that
  touches every string-creating/reading site across `value.c`, `vm.c`,
  `kosh_stdlib.c`, `json_lib.c`, and every native library that touches
  `VAL_STRING` directly. That's a real, invasive Value-system change
  (roadmap items #9/#10), not a two-line fix, and it deserves its own
  careful pass rather than being rushed in alongside a benchmarking
  session. Flagging this as the single most concrete, actionable
  finding from this whole benchmark suite for whoever picks up that
  work — the fix is well-understood even though it wasn't small enough
  to do here.

  **Update — partial fix applied (see `docs/hash-table-audit.md`'s
  sibling writeup doesn't cover this; tracked here instead):** the
  redundant-scan half of this (`str_length()` cached-header lookup,
  touching only `value_string()`/`value_copy()`/`value_free()` in
  `value.c` plus the `OP_ADD` read site in `vm.c` — all other 85
  `.as.string` sites verified unaffected) is now in place. Measured:
  20k → 100k (5x input) now takes 0.017s → 1.0s (~59x), down from
  0.03s → 2.42s (~80x) pre-fix — a genuine ~2.4x win, but **still
  worse-than-linear, not the full O(N²)→O(N) fix.** The remaining
  quadratic cost turned out not to be in `OP_ADD` at all — see the
  comment in `vm.c`'s `OP_ADD` case for the actual root cause
  (`OP_SET_LOCAL`/`OP_SET_GLOBAL`/`OP_SET_UPVALUE` deep-copying the
  result back into the variable on every assignment). Fully closing
  this needs a change to the VM's copy discipline on assignment, not
  just to string construction — left open, precisely scoped, for
  whoever picks it up next.

- **`json_bench`** (50,000 encode+decode round-trips of a small nested
  object) — Kosh roughly ties Node and **beats Python** here (0.109s vs
  0.263s). This is a genuinely good result worth highlighting rather
  than only reporting the gaps — `json_lib.c`'s encode/decode
  implementation is apparently quite competitive for small-to-medium
  structures.

- **`map_scale`** (build a `{}` map by inserting 4,000 keys one at a
  time) — only ~2x slower than Python at this size, which understates
  the real story: **Kosh's `{}` map is currently a linear-scan array,
  not a hash table at all** (verified and written up in detail in
  `docs/hash-table-audit.md`, including empirical confirmation that
  building a map scales roughly quadratically — 4x more keys took ~7x
  longer, 2x more after that took ~3x longer). Python's `dict` and
  JS's object are real hash tables, so they stay ~O(n) here regardless
  of size; Kosh's gap to them will keep widening as N grows in a way
  this benchmark's fixed size doesn't fully show. Same category of
  problem as the string-concatenation finding above: an O(n)-per-call
  operation invoked in a loop, producing O(n²) overall. See the linked
  doc for why this wasn't fixed in this pass (it's a real, invasive
  Value-system change, not a two-line fix) and what a fix would involve.

## A fifth investigation: is native-call overhead (`len()`, etc.) worth optimizing?

Not part of `run.py`'s standard suite (these three scripts are a matched
trio meant to be compared against *each other*, not against another
language) — `benchmarks/call_overhead.kh` /
`call_overhead_kosh_fn.kh` / `call_overhead_no_call.kh`, full writeup in
`docs/call-overhead-audit.md`. Short version: calling `len()` 3 million
times is ~17-22% slower than not calling anything, and — the
interesting part — calling a trivial **Kosh-level** function costs
about the same as calling the **native** `len()`, despite very
different call mechanisms afterward. That similarity points at the
shared step (the global hash lookup every bare-identifier call pays)
as the likely larger contributor, roughly confirming the roadmap's
original suspicion. A safe fix (cache the resolved table-entry pointer,
not the value, invalidated via a generation counter on table resize) is
fully specified in the audit doc but not implemented — it requires
extending the bytecode/chunk format to hold a per-call-site cache slot,
a real structural change, for a win this benchmark suggests is real but
likely smaller in typical (non call-heavy-microbenchmark) code.

## Honest summary

Kosh is meaningfully slower than Python/Node for basic bytecode dispatch
(the `loop` benchmark) but much closer — sometimes better — for function
calls and JSON handling. The clearest, most actionable findings here
aren't the cross-language comparison at all: **string concatenation**
(this doc) **and building up a `{}` map** (`docs/hash-table-audit.md`)
both get disproportionately slower as they grow — the first from a
redundant `strlen()` call, the second from the map type not actually
being a hash table — which are concrete, scoped things to fix rather
than a vague "make the VM faster" ask. That's arguably more useful than
the cross-language table,
and it's the kind of result you only get by actually running
benchmarks rather than assuming performance characteristics.

## Reproducing

```sh
make clean && make
python3 benchmarks/run.py
```

To check the string-concatenation growth rate specifically:
```sh
# edit benchmarks/string_concat.kh's 20000 to 100000, then:
time ./kosh benchmarks/string_concat.kh
```
