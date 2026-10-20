# Dispatch mechanism: computed goto vs. switch — tried, measured, reverted

Roadmap item: `benchmarks/RESULTS.md` explicitly flagged the `loop`
benchmark's dispatch overhead as the thing to try computed-goto
dispatch against first. This is the record of doing exactly that —
including a negative result, kept in the same honest-record spirit as
`hash-table-audit.md`, `trycatch-implementation.md`, and
`gc-notes.md`. **This one didn't pan out, and the code was reverted**
— but the process surfaced three real bugs worth remembering even
though the net change is a no-op for the shipped repo.

## What was built

All 44 opcode handlers in `run_loop()` (`src/vm.c`) converted from

```c
switch (op) {
    case OP_CONST: ...; break;
    ...
}
```

to

```c
static void *dispatch_table[] = { [OP_CONST] = &&LBL_OP_CONST, ... };
#define DISPATCH() goto *dispatch_table[READ_BYTE()]
...
LBL_OP_CONST: ...; DISPATCH();
```

using GCC/Clang's `&&label` extension (take a label's address; `goto
*ptr` jumps to it). The theoretical case for this — the same one
CPython's `--enable-computed-gotos` build option is based on — is that
a `switch` compiles to a single shared indirect jump instruction that
every opcode transition goes through, giving the CPU's branch
predictor one shared history slot for all of them; computed goto gives
each opcode's own `DISPATCH()` call its own indirect jump instruction,
so the predictor can learn per-opcode-pair patterns instead of one
blended pattern.

The conversion was done mechanically (an 800-line hot loop is not
something to hand-edit case-by-case) via a script that rewrote `case
OP_X:` → `LBL_OP_X:` and case-terminating `break;` → `DISPATCH();`,
built from a full read of the function first, not a blind regex pass.

## Three real bugs found while converting (all fixed before benchmarking)

1. **A nested switch inside `OP_THROW`.** The uncaught-exception
   message formatter has its own small `switch (thrown.type) { case
   VAL_STRING: ...; break; ... }` for turning the thrown value into
   readable text. Its five `break;` statements exit *that* switch, not
   the outer opcode dispatch — a blind "convert every `break;`" pass
   would have converted these too, making them jump straight to
   fetching and executing the *next opcode* from the middle of
   formatting an error message. Found by reading the function fully
   before scripting the conversion, not after; the exact line range
   was carved out and left untouched.

2. **Five opcodes read a shared `op` variable that stopped existing.**
   `OP_GET_GLOBAL`/`OP_GET_GLOBAL_WIDE` (and the same pattern for
   `SET_GLOBAL`, `DEF_GLOBAL`, `MAKE_ARRAY`, `MAKE_MAP`) share one case
   body under `switch`, disambiguating which operand-width to read via
   `(op == OP_GET_GLOBAL) ? READ_BYTE() : READ_SHORT()`. Computed goto
   has no shared `op` variable — each variant gets its own label with
   no runtime value to compare. This is a compile error (undeclared
   `op`), not a silent bug, so the compiler caught it immediately —
   but the fix needed a decision: each of the five was split into two
   fully independent handlers (one hardcoding `READ_BYTE()`, one
   `READ_SHORT()`, otherwise identical bodies) rather than trying to
   thread a shared value across a `goto` into a common continuation,
   which gets tangled with C's block-scoping rules around jumping into
   the middle of a scope. A few extra duplicated lines beats a subtle
   scoping bug.

3. **Weaker safety against a corrupted/out-of-range opcode byte.** A
   `switch` on an out-of-range value just falls through to `default:`
   — safe, by construction. A raw `dispatch_table[op]` array index
   does not get that for free; an out-of-range byte would read past
   the end of the table, undefined behavior rather than a clean
   error. Fixed by adding an `OP_COUNT` sentinel to the opcode enum
   and an explicit bounds check in `DISPATCH()` before every jump,
   restoring parity with what the switch did implicitly. (This turned
   out to matter for the performance result too — see below.)

All three were caught before any benchmarking happened. The full
existing test suite, the try/catch suite, and the GC cycle-collector
stress tests all passed against the converted build, under
AddressSanitizer and in a plain `-O2` production build, with zero
regressions.

## The measurement — and why the change was reverted

Median of 5-7 runs each, switch-based vs. computed-goto, same `-O2`
build flags, same machine, back to back:

| Benchmark | switch (shipped) | computed goto | computed goto, no bounds-check |
|---|---|---|---|
| `loop.kh` (5M iterations) | 1.00s | 1.12s | 1.08s |
| `fib.kh` (`fib(28)`) | 0.152s | 0.175s | — |

**Computed goto was slower, not faster, on both benchmarks tested** —
roughly 5-12% slower on `loop`, ~15% slower on `fib`. Removing the
`OP_COUNT` bounds check (item 3 above) recovered some of the gap but
not all of it — computed goto without any safety check was *still*
slower than the switch. This was checked with more repetitions
specifically because the first result was surprising; it held up.

The honest interpretation: GCC already compiles a dense small-integer
`switch` like this one into a jump table at `-O2` — the "single shared
indirect branch" framing that motivates computed goto isn't really
what's happening here, the switch is closer to computed-goto's own
mechanism than the classical argument assumes. On top of that, both
benchmarks have short, highly *regular* opcode-transition sequences
(the same handful of opcodes cycling in a fixed pattern, 5,000,000
times for `loop`) — close to the best case for a modern CPU's indirect
branch predictor to learn a single shared jump's target pattern well,
which is exactly the case where computed goto's "separate predictor
slot per opcode" advantage matters least. The classical CPython result
this idea is based on comes from a much larger, more varied bytecode
set with less locally-repetitive dispatch patterns; it doesn't
generalize here.

**Reverted.** The switch-based `vm.c` that's actually in this repo is
the one that measures faster. Shipping the computed-goto version would
have added real code complexity and the three bug classes above for a
net *regression*, not a win — not a reasonable trade.

## What this means for anyone revisiting performance

- Dispatch mechanism is **not** where Kosh's per-operation overhead
  comes from, at least not on GCC at `-O2`. Don't re-try this exact
  change without a real reason to expect a different result (a
  different compiler, a very different, much larger and less regular
  opcode mix than these two benchmarks exercise).
- `benchmarks/RESULTS.md`'s own standing hypothesis — that
  `value_copy()` running on every `OP_ADD`/`OP_GET_LOCAL`/
  `OP_SET_LOCAL`, even for a plain number, is a meaningful chunk of
  the ~194x-vs-C gap — was **not** touched by this work and remains
  the most promising next thing to actually profile (not just
  hypothesize about) if someone picks up performance again.
