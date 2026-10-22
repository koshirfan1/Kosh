# Call Overhead / Inline Caching Audit

Roadmap item #6 specifically calls out: "Agar `len()` hai to unnecessary
lookup avoid karo" (if it's `len()`, avoid the unnecessary lookup) —
implying native built-in calls pay an avoidable global-hash-table-lookup
cost that should be short-circuited. This is an investigation of whether
that's actually the bottleneck, and if so, what a *safe* fix looks like.

## Measurement methodology

Three matched benchmark scripts (`benchmarks/call_overhead*.kh`), same
loop shape, same iteration count (3,000,000), differing only in what
happens each iteration:

1. `call_overhead.kh` — calls a **native** function (`len(arr)`)
2. `call_overhead_kosh_fn.kh` — calls a **Kosh-level** function
   (`get_five()`) that does the same "return a value" job
3. `call_overhead_no_call.kh` — no call at all (`total = total + 5`)

The comparison that matters: (1) and (2) **both** require an
`OP_GET_GLOBAL` hash-table lookup by name to find the callee (the
compiler always does this for a bare-identifier call — see
`docs/opcodes.md`'s calling-convention section). They differ in *what
happens after* the lookup: (1) directly invokes a C function pointer;
(2) sets up a full `CallFrame`, runs Kosh bytecode, and returns through
`OP_RETURN`'s locals-freeing pass (`docs/memory-notes.md`). If the
lookup were the dominant cost, (1) and (2) should look similar to each
other and both clearly slower than (3). If the *call mechanism* were
dominant, (1) and (2) should differ meaningfully from each other.

5-6 trials each, outliers trimmed (one clear system-noise spike
discarded — a 1.5s reading against a baseline of ~0.85-0.95s on an
otherwise-consistent run):

| scenario | median time (3M iterations) |
|---|---|
| `native_call` (`len`) | 0.853s |
| `kosh_call` (`get_five`) | 0.890s |
| `no_call` (baseline) | 0.730s |

## Interpretation

Both call scenarios are **~17-22% slower** than the no-call baseline,
and — this is the important part — **close to each other** despite
having very different call mechanisms after the lookup (a native calls
one C function directly; a Kosh function pays full frame setup +
`OP_RETURN` cleanup). That similarity is the signal: since the *shared*
step (the global hash lookup) is what both scenarios have in common,
and they cost about the same despite differing afterward, the lookup —
not the call mechanism — looks like the larger contributor to the
~17-22% gap. This is consistent with (though this benchmark alone can't
fully prove) the roadmap's original suspicion.

**Caveat on generalizability**: this is a deliberately call-heavy
microbenchmark (one call per loop iteration, doing almost no other
work). A real program that does meaningful work per call — string
processing, array manipulation, actual business logic — would see a
much smaller *relative* share of its time going to the lookup, since
the lookup cost is fixed per call while the surrounding work scales
with what the call actually does. Treat "17-22%" as a ceiling for
call-dominated code, not a general claim about typical Kosh programs.

## Why this wasn't fixed here

The obvious fix — cache the resolved value at each call site so
repeated calls to the same name skip the hash lookup — has a real
correctness hazard: Kosh globals are mutable (`OP_SET_GLOBAL` can
reassign `len` to something else entirely, unusual as that would be).
A naive value-cache would silently keep calling the *old* function
after such a reassignment.

**A safe version is well-understood, though, and worth specifying
precisely for whoever picks this up:**

Cache the resolved **`TableEntry*` pointer**, not the value, at each
`OP_GET_GLOBAL` call site. Reading `entry->val` at call time (rather
than a snapshotted copy) means a subsequent `OP_SET_GLOBAL` reassigning
that name is still correctly reflected — `table_set()` mutates the
entry in place for an existing key, it doesn't relocate it. The only
way a cached pointer goes stale is a **table resize** (rehashing
moves every entry to a new backing array — see `table_set()`'s 75%-
load-factor growth check in `docs/hash-table-audit.md`'s writeup of
the same `Table` type). That's a rare event in practice: nearly all of
a real program's distinct global names (every top-level `let`/`fn`,
including every nested-function redefinition — see the `OP_DEF_GLOBAL`
note in `docs/opcodes.md`) get declared once, early, before any hot
loop starts calling them repeatedly — so by the time a cache would
actually pay off, the table has typically already stopped growing.

Concretely: give `Table` a `generation` counter, incremented on every
resize. Each `OP_GET_GLOBAL`/`OP_GET_GLOBAL_WIDE` call site gets a
small cache (cached `TableEntry*` + the `generation` value it was
cached under) — this needs a place to live per call site, which is the
part requiring real design work (`Chunk` doesn't currently have a
parallel per-instruction cache slot; adding one is itself a change
that touches the bytecode format `docs/opcodes.md` describes). On each
execution: if the cached generation matches the table's current
generation, use the cached pointer directly (skip `hash_string()` +
probing entirely); otherwise, do the full lookup and refresh the cache.
This is a standard, well-tested technique (used in real production
dynamic-language VMs) — safe, but a genuinely invasive change to the
bytecode/chunk format, not a two-line fix.

**Given a measured-but-uncertain (17-22% ceiling, likely less in
typical code) win against a real structural change to the bytecode
format** — the same risk/reward calculus applied to the string and map
quadratic-growth findings — this is documented as a precise, actionable
spec rather than implemented in this pass. Re-run
`benchmarks/call_overhead*.kh` before and after any future attempt to
confirm the fix actually delivers the expected win before merging it.
