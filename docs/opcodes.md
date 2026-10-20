# Kosh Bytecode — Opcode Reference

Generated from `src/chunk.h` (the enum) and `src/vm.c` (`run_loop`'s
dispatch switch, which is the authoritative behavior — treat this doc as
a guide to reading that switch, not a replacement for it). 40 opcodes
total; 2 are reserved/unused (noted below).

Every instruction is a single opcode byte, optionally followed by a
fixed-size operand. "Stack effect" uses `[before] -> [after]`, top of
stack on the right.

## Constants & literals

| Opcode | Operand | Stack effect | Notes |
|---|---|---|---|
| `OP_CONST` | 1 byte: constant-pool index | `[] -> [value]` | For chunks with ≤256 constants |
| `OP_CONST_WIDE` | 2 bytes (big-endian): constant-pool index | `[] -> [value]` | Used once a chunk's constant pool exceeds 256 entries |
| `OP_NIL` | — | `[] -> [nil]` | |
| `OP_TRUE` | — | `[] -> [true]` | |
| `OP_FALSE` | — | `[] -> [false]` | |

## Arithmetic & comparison

All binary; pop two operands, push one result. Numeric-only except
`OP_ADD`, which also handles string concatenation.

| Opcode | Stack effect | Notes |
|---|---|---|
| `OP_ADD` | `[a, b] -> [a+b]` | Number+number or string+string; mixed types are a runtime error |
| `OP_SUB` | `[a, b] -> [a-b]` | |
| `OP_MUL` | `[a, b] -> [a*b]` | |
| `OP_DIV` | `[a, b] -> [a/b]` | Always float division (Values are doubles; no separate int type) |
| `OP_MOD` | `[a, b] -> [a%b]` | |
| `OP_NEGATE_NUM` | `[a] -> [-a]` | Unary |
| `OP_EQ` | `[a, b] -> [a==b]` | |
| `OP_NEQ` | `[a, b] -> [a!=b]` | |
| `OP_LT` / `OP_LE` / `OP_GT` / `OP_GE` | `[a, b] -> [bool]` | |
| `OP_NOT_BOOL` | `[a] -> [!a]` | Unary |

## Variables

| Opcode | Operand | Stack effect | Notes |
|---|---|---|---|
| `OP_GET_LOCAL` | 1 byte: stack-slot index, relative to the current frame's `slots` base | `[] -> [value]` | Pushes `value_copy()` of the slot — always a fresh reference, not aliased to the original |
| `OP_SET_LOCAL` | 1 byte: slot index | `[value] -> [value]` | Assigns without popping (assignment is itself an expression) |
| `OP_GET_GLOBAL` / `OP_GET_GLOBAL_WIDE` | 1 or 2 bytes: constant-pool index of the name string | `[] -> [value]` | Looked up by name in the VM's global hash table at runtime |
| `OP_SET_GLOBAL` / `OP_SET_GLOBAL_WIDE` | same | `[value] -> [value]` | Reassignment of an *existing* global; frees the old value first |
| `OP_DEF_GLOBAL` / `OP_DEF_GLOBAL_WIDE` | same | `[value] -> []` | First-time declaration — also how every `fn` (including nested ones) gets bound; see the function-registry special case below |
| `OP_GET_UPVALUE` | 1 byte: upvalue-slot index | `[] -> [value]` | Reads a captured closure value — see "Closures" below |
| `OP_SET_UPVALUE` | 1 byte: upvalue-slot index | `[value] -> [value]` | Writes to a captured closure value (updates *this closure's own copy* — does not propagate back to the enclosing function's variable; capture is by-value, not a live reference) |

### `OP_DEF_GLOBAL`'s function-registry special case

If the value being defined is a `VAL_NUMBER`, the VM checks whether that
number is a valid index into the compile-time function registry
(`fn_registry`). If so, this isn't really "define a number global" — the
compiler encoded "define a function" as pushing its registry index, to
avoid embedding a full function object as a bytecode constant. The VM
reconstructs a proper `VAL_FUNCTION` value here, and — if the function
has any upvalues — snapshots the current frame's captured values into a
new `KhanClosure` at this exact moment. This is also why a nested `fn`
declared inside a function that's called repeatedly redefines the same
global name every call (see `docs/memory-notes.md` for why that mattered
for a real leak that was fixed here).

## Control flow

| Opcode | Operand | Behavior |
|---|---|---|
| `OP_JUMP` | 2 bytes: forward offset | Unconditional. Used for `if`/`elif`/`else` branching and as the mechanism `break`/`continue` compile down to (there's no dedicated break/continue opcode — see below) |
| `OP_JUMP_IF_FALSE` | 2 bytes: forward offset | Pops nothing — peeks the top of stack; jump taken if falsy. The condition value itself is popped separately by the surrounding `if`/`while` bytecode on both branches |
| `OP_LOOP` | 2 bytes: backward offset | Unconditional backward jump — how `while`/`for` loop back to their condition check |
| `OP_BREAK` | — | **Reserved, unused.** `break` compiles to a plain `OP_JUMP` targeting just past the loop, with locals popped inline first (see `LoopCtx` in `compiler.c`) |
| `OP_CONTINUE` | — | **Reserved, unused.** Same story — `continue` compiles to `OP_JUMP` targeting the loop's condition re-check (`while`) or increment step (`for`), locals popped inline first |

## Functions

| Opcode | Operand | Stack effect | Notes |
|---|---|---|---|
| `OP_CALL` | 1 byte: argument count | `[callee, arg1..argN] -> [result]` | See "Calling convention" below |
| `OP_RETURN` | — | `[..., retval] -> []` (in caller: `[] -> [retval]`) | Frees every value still live in the current frame's stack region (including the callee slot) before returning — this used to be a leak source, see `docs/memory-notes.md` |

### Calling convention

For a call with N arguments, the stack immediately before `OP_CALL`
executes is `[..., callee, arg1, arg2, ..., argN]`. The new frame's
`slots` pointer is set to `stack_top - N`, i.e. **local slot 0 is the
first argument**, not the callee — the callee sits one slot *before*
`frame->slots`. (This was the subject of a fixed off-by-one bug in an
earlier pass — worth being careful about if touching this code, since
getting it wrong silently shifts every parameter by one rather than
crashing.)

## Collections & indexing

| Opcode | Operand | Stack effect | Notes |
|---|---|---|---|
| `OP_MAKE_ARRAY` | 1 byte: element count | `[e1..eN] -> [array]` | Elements popped in reverse, so the resulting array preserves source order |
| `OP_MAKE_ARRAY_WIDE` | 2 bytes: element count | `[e1..eN] -> [array]` | Used once a literal has more than 255 elements — see `docs/ast-audit.md` for the silent-truncation bug this fixes (the 1-byte form used to just wrap around with no error) |
| `OP_MAKE_MAP` | 1 byte: pair count | `[k1,v1..kN,vN] -> [map]` | |
| `OP_MAKE_MAP_WIDE` | 2 bytes: pair count | `[k1,v1..kN,vN] -> [map]` | Same as `OP_MAKE_ARRAY_WIDE`, for map literals |
| `OP_GET_INDEX` | — | `[collection, key] -> [value]` | Works on both arrays (numeric index) and maps (string key) |
| `OP_SET_INDEX` | — | `[collection, key, value] -> [value]` | |

## Misc

| Opcode | Operand | Stack effect | Notes |
|---|---|---|---|
| `OP_PRINT` | — | `[value] -> []` | |
| `OP_POP` | — | `[value] -> []` | Frees the popped value — this is how block-scope locals get cleaned up at the end of every `if`/`while`/`for` body |

## Not yet covered by this doc

- Native function calls (`http_get`, `len`, etc.) — these are plain
  `VAL_NATIVE` values retrieved via the same `OP_GET_GLOBAL`/
  `OP_GET_LOCAL` + `OP_CALL` path as any other function; there's no
  separate "call native" opcode. Worth a follow-up pass to document the
  `NativeFn` C signature and argument-marshaling contract for anyone
  writing a new native library.
- Exact byte layout / endianness worked out above from `READ_SHORT`/
  `emit_short` in `vm.c`/`compiler.c` (big-endian, 2 bytes) — not yet
  cross-checked against every emission site for consistency.
- No opcode-count/compactness audit yet (see roadmap #5) — this doc
  describes what exists, not whether it's optimal.
