# try / catch / throw — implementation notes

Roadmap item #4 (exception handling, previously ❌ Not started). This
covers the design, what changed, and a real bug found and fixed along
the way — kept here in the same spirit as `hash-table-audit.md` and
`ast-audit.md`: an honest record, not just "done ✅".

## Syntax

```
try:
    <statements>
catch e:
    <statements — `e` is bound to whatever was thrown>

try:
    <statements>
catch:
    <statements — caught value is discarded, unbound>

throw "some message"      # any value can be thrown, not just strings
throw {"code": 404}
```

`catch` is required — there's no bare `try` without one. Binding a
name is optional. `try`/`catch` can nest, and both can wrap function
calls (including recursive ones) — an exception thrown several call
frames deep unwinds correctly back to the nearest enclosing handler
within the same script.

## Design

- **Lexer/parser/AST**: three new keywords (`try`, `catch`, `throw`),
  one new AST node (`AST_TRY_STMT`, holding a try-block, optional
  catch-variable name, and catch-block — mirrors `AST_IF_STMT`'s
  shape), and `AST_THROW_STMT` (reuses the same `data.expr` slot
  `AST_PRINT_STMT`/`AST_RETURN_STMT` already use).

- **Compiler**: `try`/`catch` reuses `emit_jump()`/`patch_jump()`
  completely unchanged — `OP_TRY_BEGIN` takes the exact same 2-byte
  jump-offset encoding as `OP_JUMP`. It doesn't jump on entry; it just
  registers where the catch block starts, then falls through into the
  try-block's own bytecode. `OP_TRY_END` retires that registration on
  a normal (non-throwing) completion, followed by a plain `OP_JUMP`
  that skips over the catch block. The catch variable binding mirrors
  `AST_LET_STMT`: the caught value is already sitting on top of the
  stack (pushed by the VM during unwind) by the time the catch block's
  bytecode starts, so `add_local()` just claims that slot — no extra
  emit needed. A bare `catch:` emits one `OP_POP` instead to discard it.

- **VM**: a small handler stack (`VM_TRY_MAX = 64`) of
  `{catch_ip, stack_top, frame_count}`, pushed by `OP_TRY_BEGIN`. On
  either `OP_THROW` or any existing `runtime_error()` call (division
  by zero, undefined variable, out-of-bounds index, etc. — every one
  of the ~13 existing call sites, not just new user-facing `throw`),
  if a handler is active: pop it, free every value between its saved
  `stack_top` and the current one (this covers freeing the locals of
  any call frames being unwound too, for free — a frame's `slots` is
  just a pointer into this same shared stack array, no separate walk
  needed), rewind `vm->stack_top`/`vm->frame_count` to the handler's
  saved values, jump `ip` to `catch_ip`, and push the thrown/error
  value for the catch block to bind or discard.

## Known limitation

Exceptions thrown from inside a **native-callback context** (e.g. a
comparator passed to a sort function, or an `array.map()` callback)
aren't catchable by a handler in an *enclosing* scope, if that native
function re-enters the VM via `vm_call_fn()`'s separate, recursive
`run_loop()` C call rather than the normal in-loop `OP_CALL` path
(pushing a new `CallFrame` and continuing the *same* `run_loop`
invocation, which is how ordinary Kosh function calls — including
recursion — work, and which unwinding handles correctly). This is a
narrower case than it might sound: only native functions that invoke
a Kosh closure as a callback hit the recursive path at all. It fails
safe, not silently — the inner `run_loop()` returns
`INTERPRET_RUNTIME_ERROR` same as before try/catch existed, so the
enclosing native call gets a clean nil back and the program keeps
running; the outer `try` just doesn't catch it. Flagging precisely so
nobody has to re-derive this if it comes up.

## Bug found and fixed: `continue` inside `do { ... } while(0)`

The first working version wasn't quite working: a **single** try/catch
worked fine, but **two sequential** try/catch statements in the same
script produced wrong catch values — the second block's caught
variable showed the *first* block's error message instead of its own.

Root cause: the ~13 existing `runtime_error()` call sites all needed
to become catch-aware — on a caught error, control must resume the
dispatch loop at the catch block instead of returning out of
`run_loop()` entirely. That logic was wrapped in a `TRY_ERR(msg)`
macro using the standard `do { ... } while(0)` pattern (so the macro
behaves like one statement even when used inside a bare, brace-less
`if`). The macro's last step was `continue;`, intended to resume
`run_loop`'s `for (;;)` dispatch loop.

**`continue` in C always targets the *nearest* enclosing loop — and
`do { ... } while(0)` is a loop**, even though it only ever runs once.
So `continue` inside the macro didn't resume the dispatch loop at
all — it "continued" the `do-while(0)` itself, which then immediately
exited (its condition is `0`), falling through to whatever code came
*after* the macro invocation in the original call site. For
`OP_DIV`'s zero-check specifically:

```c
if (b.as.number == 0.0) { value_free(a); value_free(b); TRY_ERR("Division by zero"); }
push(vm, value_number(a.as.number / b.as.number));   /* <- ran anyway! */
```

`a`/`b` were already freed but their `.as.number` bits were untouched
(freeing a `VAL_NUMBER` is a no-op on the number itself), so
`a.as.number / b.as.number` re-ran the same zero-division, and its
`inf` result got pushed onto the stack anyway — one extra, unaccounted
slot sitting on top of the correctly-caught value. Every catch block
compiled afterward still worked from the compiler's assumed slot
layout, but the *runtime* stack was now permanently one slot deeper
than the compiler thought — so the next try/catch's catch variable
read one slot low, landing on the leftover value from the previous one.

Found via opcode-level tracing (a temporary instrumented build logging
every opcode + stack pointer + a print inside the unwind path) — the
thrown value was confirmed being pushed at the *correct* address by
the unwind itself, but the very next traced instruction already showed
the stack one slot deeper than expected, which is what pointed at an
extra, unaccounted push between those two points rather than anything
wrong with the unwind logic itself.

**Fix:** replaced the macro's `continue;` (and the direct `continue;`
in `OP_THROW`'s own case, for consistency, even though that one
wasn't actually broken — it's directly inside the `switch`/`for`, not
wrapped in anything) with `goto dispatch_loop_top;`, jumping to a
label placed at the very top of `run_loop`'s `for (;;)`. `goto` isn't
subject to the same nearest-enclosing-loop capture that `continue`/
`break` are, so it reaches the real dispatch loop regardless of what
it's nested inside.

Verified after the fix: two, three, and four sequential try/catch
blocks; try/catch re-entered repeatedly inside a `while` loop; nested
try/catch (including a `throw` from inside a `catch` block, caught by
an outer handler); exceptions crossing recursive function calls; and
the full existing test suite (`tests/suites/*.kh`, `test_map_assign.kh`,
`test_json_db.kh`, `examples/*.kh`) — all pass, including the ordinary
(non-try) division-by-zero and other runtime-error paths, which now
correctly print their message and terminate with exit code 70 exactly
as they did before try/catch existed, when no handler is active.
