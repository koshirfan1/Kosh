# Memory Audit Notes

Findings from a systematic valgrind/ASAN pass over the VM's core value
lifecycle (function returns, global (re)definition, and process-exit
teardown), plus what's still an open, expected one-time cost.

## Fixed: three real, runtime-scaling leaks

All three were found the same way — write a tight loop stress test (1000
iterations of recursion / array-building / closure-creation), run it
under `valgrind --leak-check=full`, and check whether the leaked byte
count *scales* with iteration count. A leak proportional to loop count is
a real, unbounded-growth bug for any long-running program (e.g. `webi`'s
server loop); a fixed one-time count is normal interpreter startup
overhead. All three below scaled with iterations before the fix, and are
flat (zero growth) after it — verified at both 200 and 400 iterations
producing byte-for-byte identical leak totals.

### 1. `OP_RETURN` discarded locals without freeing them (`src/vm.c`)

When a function returned, the VM freed nothing between `frame->slots`
and the old `stack_top` — it just moved the stack pointer down
(`vm->stack_top = frame->slots - 1`). Any local variable still "in
scope" at the point of `return` (which is essentially always true —
even a function's own final `return` returns from inside its outermost
block, before that block's natural end-of-scope cleanup would run) had
its value abandoned without a `value_free()` call. For scalars this is
harmless; for arrays, maps, strings, or closures, this leaked the
underlying allocation every single call.

This also covered the **callee slot** itself (`frame->slots - 1`, where
`OP_CALL` places a retained copy of the function/closure value being
invoked) — previously abandoned on every call, leaking at minimum the
callee's name string, and for a closure, its captured values too. This
is why calling a returned closure repeatedly (`f(1)` in a loop) leaked
independently of the closure's own creation.

Fixed by freeing every slot in `[frame->slots - 1, stack_top)` (or
`[frame->slots, stack_top)` for the top-level script frame, which has
no callee slot below it) before truncating. `result` itself is exempt
since it's already been popped off the stack by this point.

The same missing-free existed in `vm_call_fn`'s error-cleanup path
(native → Kosh call bridge, when the called function errors) — fixed
identically, though this path matters less since a runtime error
usually means the process is about to exit anyway.

### 2. `OP_DEF_GLOBAL` never freed the value it was replacing (`src/vm.c`)

`OP_SET_GLOBAL` (plain reassignment, `x = 5`) already correctly freed
the existing value before overwriting. `OP_DEF_GLOBAL` (first-time
declaration) didn't — reasonable on its own, since a top-level `let`
normally only runs once. But nested `fn` declarations *also* compile to
`OP_DEF_GLOBAL` (see the design note below), and a nested function
declared inside a function that gets called repeatedly redefines the
same global name every single call. Without freeing the old value
first, every call after the first leaked the previous closure and
everything it had captured.

Fixed by mirroring `OP_SET_GLOBAL`'s existing pattern: check for an
existing value under that name and free it before `global_set()`.

### 3. `table_free()` freed each global's key but not its value (`src/vm.c`)

Used once, at process exit (`vm_free()` → `table_free(&vm->globals)`).
This doesn't cause unbounded growth during execution — it only means
whatever was still alive in the globals table at the moment the script
ended was never freed before the process exits. But it made every
valgrind run on a script with any live global state at exit report a
large "indirectly lost" number that had nothing to do with actual
leaks, and it's a trivial, safe fix for complete teardown. Fixed by
freeing each entry's value alongside its key.

## Known, deliberate: nested `fn` compiles as a global, not a local

Not a memory bug, but adjacent enough to note here since it's what
surfaced bug #2 above. A nested function declaration:

```
fn make_adder(x):
    fn adder(y):
        return x + y
    return adder
```

compiles `adder` as a **global** variable (`OP_DEF_GLOBAL`), not a local
scoped to `make_adder`. In practice this is safe for the only pattern
that appears anywhere in this codebase — declare, then immediately
return or call the nested function within the same invocation — because
closures carry their own direct reference to their specific compiled
function + captured values, not a "look up by name" indirection.
Tested explicitly: two different outer functions each declaring a
nested function named `helper`, both closures kept alive and called
after both outer functions have run, each still calls the *correct*
version. So this isn't presently causing incorrect behavior.

It's worth flagging for whoever works on compiler architecture (#3/#4 on
the roadmap) as something to give nested functions a real local scope
rather than a global one, since the current behavior is correct by
coincidence of usage pattern, not by design guarantee.

## Remaining, expected: one-time startup/compile bookkeeping

After the three fixes above, every remaining valgrind-reported leak is a
**fixed count independent of runtime/loop iterations** — confirmed by
running the same script at 200 and 400 loop iterations and getting
byte-for-byte identical leak totals both times. These come from:

- Parser: AST nodes for the compiled program (never freed after compile)
- Compiler: `KoshFunction` registry entries, constant-pool string
  interning (`strdup` calls in `emit_global_get`/`emit_const` etc.)
- `vm_register_builtins()` / `*_register_all_vm()`: each native
  function's name string, `strdup`'d once at startup and never freed

This is deliberate skip-teardown behavior common in short-lived script
interpreters (the OS reclaims everything at exit; walking every parser/
compiler structure just to free it before `return 0` costs real
complexity for zero externally-visible benefit in a one-shot script
run). It only becomes worth revisiting if:
- Kosh ever needs to support running many independent scripts within
  one long-lived process (a real GC/teardown pass would then matter), or
- Someone wants perfectly clean valgrind output for its own sake.

Neither is true today, so this is left as-is and documented rather than
"fixed" — see the CI memory-check job's comment for why it's set to
`continue-on-error: true` rather than a hard merge gate.

## 10-million-allocation stress test (the roadmap's literal ask for #7)

`tests/fuzz/memory_stress_10m.py` runs three real Kosh scripts under
live RSS (resident memory) monitoring via `psutil`, sampling every
0.5s:

1. **Uniform**: 10,000,000 iterations creating a single-key map each
   time (the roadmap's literal "10 million allocations").
2. **Variable**: 2,000,000 iterations creating arrays of varying size
   (0-96 elements, cycling) — uniform-size allocations alone can't
   reveal fragmentation, since a freed slot of the same size is always
   immediately reusable; this scenario forces the allocator to deal
   with a mix of sizes.
3. **Closures**: 10,000,000 iterations of `make_adder(i)` + calling the
   result — the exact shape of the bug found and fixed earlier in this
   document (`OP_DEF_GLOBAL` not freeing on redefinition), now stress-
   tested at the actual scale the roadmap asked for, not just the
   200-vs-400-iteration comparison used to originally find and confirm
   the fix.

All three: RSS climbs briefly in the first ~0.5s (allocator pool /
malloc arena warmup) and then **stays completely flat for the rest of
the run** — literally identical MB readings sample after sample, all
the way to completion (10M map allocs in ~3.5-4s, 2M variable-size
allocs in ~2s, 10M closure creations in ~4-4.5s). No fragmentation-
driven growth, no leak-driven growth, at the actual scale requested.

This runs in CI (`memory-check` job, informational/non-blocking like
the valgrind job above it) using the release build rather than the
debug build valgrind needs — the whole point of this check is
production-realistic timing at real scale, which a debug/valgrind
build's 10-50x slowdown would make impractical to run on every push.

## How to reproduce this audit

```sh
gcc -std=c11 -g -O0 -Isrc -D_POSIX_C_SOURCE=200809L \
  -Wno-implicit-function-declaration -Wno-builtin-declaration-mismatch -Wno-cast-function-type \
  src/lexer.c src/parser.c src/ast.c src/chunk.c src/value.c src/compiler.c src/vm.c \
  src/vm_libs.c src/interpreter.c src/kosh_stdlib.c src/json_lib.c src/datetime_lib.c \
  src/requests_lib.c src/webi_lib.c src/sqlite_lib.c src/vision_lib.c src/vision_cv.c \
  src/vision_cascade.c src/main.c -o kosh_dbg -lm

valgrind --leak-check=full --show-leak-kinds=all ./kosh_dbg your_script.kh
```

To confirm whether a reported leak scales with runtime rather than being
fixed startup cost: run the same script with the loop count doubled and
diff the leak totals. Flat = expected. Doubled = real bug.
