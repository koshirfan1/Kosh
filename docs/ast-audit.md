# AST Audit

Roadmap item #3 asked for an audit of AST node independence, in service
of future optimization passes. This is that audit — and it turned up
one serious correctness bug and one real performance bug along the way.

## Node independence: confirmed good

- **Exhaustive node-type handling**: `ast_free()`'s switch covers all
  29 `AstNodeType` values with no `default:` case — confirmed
  exhaustive by the fact that `-Wswitch` (part of `-Wall`, already used
  in every build in this project) would warn on a missing case, and
  never has.
- **String ownership**: every AST constructor that takes a `const
  char *` (`ast_new_string`, `ast_new_identifier`, `ast_new_assignment`,
  `ast_new_call`, `ast_new_let_stmt`, `ast_new_for_stmt`,
  `ast_new_fn_decl`, `ast_new_import_stmt`, `ast_new_from_import_stmt`)
  `strdup()`s its input — verified individually, all nine. No node
  aliases a caller's buffer (typically a `Token`'s pointer into the
  transient source-text buffer), so nothing can dangle once parsing
  finishes and that buffer's lifetime ends.
- **Union zero-initialization**: `ast_new_node()` `memset()`s the
  entire `data` union to zero before any specific constructor fills in
  its fields — meaning even a hypothetical future constructor that
  forgets to set some field defaults to `NULL`/`0` rather than
  uninitialized garbage. Good defensive design, already in place.

## Found: array/map literals over 255 elements silently produced wrong results

Not a node-independence issue exactly, but found while tracing how
`AST_ARRAY`/`AST_MAP` get compiled (`compiler.c`) — worth documenting
here since it's exactly the kind of thing an AST-to-bytecode audit
should catch.

`OP_MAKE_ARRAY`/`OP_MAKE_MAP`'s element/pair count was encoded as a
single byte (`(uint8_t)count`) with no wide variant, unlike
`OP_CONST`/`OP_GET_GLOBAL`/`OP_DEF_GLOBAL` (all of which have a
`_WIDE` 2-byte-operand sibling for exactly this situation — see
`docs/opcodes.md`). A literal with more than 255 elements silently
wrapped: **verified empirically that a 5,000-element array literal
reported `len() == 136`** (5000 mod 256 = 136, confirming the exact
truncation mechanism), and a 10,000-element one reported `16` (10000
mod 256 = 16). No error, no crash — just a silently wrong length, and
every element past the wrapped-around count permanently inaccessible
via normal indexing even though it's still sitting in memory.

Fixed by adding `OP_MAKE_ARRAY_WIDE`/`OP_MAKE_MAP_WIDE` (2-byte operand,
big-endian, matching the existing `_WIDE` convention exactly), emitted
by the compiler once a literal exceeds 255 elements/pairs, with a
compile error (not silent truncation) if a literal somehow exceeds
65,535. Verified: 255 and 256-element literals both now produce
correct results (the boundary where the format switches), a
5,000/10,000-element literal now correctly reports its real length,
and — since building an array/map literal needs every element sitting
on the VM's value stack before the `MAKE_ARRAY`/`MAKE_MAP` instruction
consumes them — a literal large enough to exceed the value stack's own
16,384-slot limit (`docs/memory-notes.md`'s stack-overflow fix)
correctly and safely reports a clean stack-overflow error rather than
either silently truncating or corrupting memory. Two independent
safety mechanisms working together correctly.

Regression tests added: `tests/suites/arrays_maps.kh` now includes a
genuine 300-element array *literal* (not built via `push()` in a loop,
which goes through a completely different code path and wouldn't have
exercised this bug at all) checking its length and first/last elements.

## Found: building an AST list was O(n²)

`ast_list_append()` (used for every statement list, argument list,
array/map literal element list, function parameter list — anywhere the
grammar has a repeated `a, b, c, ...` or a sequence of statements)
walked the **entire list from the head** on every single call to find
the tail:

```c
AstNodeList *cur = list;
while (cur->next) cur = cur->next;
cur->next = item;
```

Building an N-element list one append at a time is therefore
`1 + 2 + 3 + ... + N` = O(n²) total, independent of anything else
going on. Verified empirically with a large array literal (isolating
parse cost from execution cost, since a large literal is parsed once
but only evaluated once too — unlike, say, N separate top-level `let`
statements, which have their own separate per-statement execution cost
that would confound a timing comparison): 5,000 → 10,000 elements (2x)
took ~3.9x longer, 10,000 → 20,000 (2x again) took ~4.1x longer — both
almost exactly the 4x a doubling should show under pure O(n²), a very
clean quadratic signature.

This is the same architectural category of bug as the string-
concatenation and map-construction findings from earlier sessions
(`docs/memory-notes.md`, `docs/hash-table-audit.md`) — something O(n)
per call, called in a loop. Unlike those two, though, this one was
**low-risk to fix directly** rather than needing a larger Value-system
change: added a `tail_cache` field to `AstNodeList`, meaningful only on
the head element every caller actually holds a reference to (the
`list = ast_list_append(list, node)` calling pattern already used
everywhere), updated on every append. This makes each append O(1)
(amortized), and the whole list O(n) to build instead of O(n²).
Included a defensive fallback (full walk) if the cache is ever missing,
though nothing else in the codebase constructs an `AstNodeList` node
directly — verified via a full-codebase grep — so this should never
trigger.

**Verified fixed**: the same 5,000/10,000/15,000-element array-literal
timing that showed clear quadratic growth before the fix now shows
flat, sub-10ms timing at every size tested. Full regression suite (139
assertions + 137-assertion webi suite + parser fuzz suite, 1000+
mutations) stays green, and a dedicated ASAN pass over the changed
code shows no new memory-safety issues.

## What this means for future compiler optimization work

Since this was explicitly framed as groundwork for future optimization
passes (roadmap #4's remaining dead-code-elimination/constant-
propagation/peephole items): the AST is now confirmed to have clean,
independent node ownership (safe to transform/rewrite subtrees without
fear of aliasing) and a properly-linear list-construction cost (so an
optimization pass that needs to build new lists, e.g. a dead-code
elimination pass reconstructing a statement list with some statements
removed, won't itself introduce quadratic behavior via the same
`ast_list_append` pattern this audit just fixed).
