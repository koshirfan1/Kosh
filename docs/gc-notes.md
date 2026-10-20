# Cycle collector — implementation notes

Roadmap item: garbage collector (circular reference leaks). Same
honest-record spirit as `hash-table-audit.md` and
`trycatch-implementation.md` — two real, serious bugs were found
during testing, not just "done ✅", and both are worth remembering.

## The problem

Arrays and maps are the only heap values in Kosh that are genuinely
**shared** — `value_copy()` increments `ref_count` rather than
deep-copying (see `value.c`). That's what makes a reference cycle
possible:

```
let a = []
a = push(a, 1)
a[0] = a          # a now references itself
```

(Note: `push()` specifically does **not** create this — it builds a
brand new array rather than mutating in place. Genuine in-place
mutation needs index-assignment, `a[i] = ...`.) Once every *external*
reference to a cycle like this is gone, plain refcounting can never
free it — nothing ever decrements the last ref, because the cycle is
decrementing itself. This was a named, open weakness before this work
(`ROADMAP_STATUS_UPDATED.md` marked garbage collection ❌ Not started).

## Why trial deletion, not a tracing GC

This implements Bacon & Rajan's **synchronous trial-deletion cycle
collector** — the same algorithm CPython's `gc` module uses for its
container types — rather than a full stop-the-world tracing GC over
the whole VM.

The key advantage for this codebase specifically: trial deletion only
needs to walk the **array/map object graph itself** (each object's own
children). It never needs to know about the VM's stack, call frames,
globals table, upvalues, or the try/catch handler stack — those are
already visited correctly by the existing, ordinary refcount
decrements. A full tracing collector would need every one of those
enumerated as GC roots, correctly, and kept correct as the VM changes,
to avoid ever freeing something still live. Trial deletion sidesteps
that entire integration surface by only ever considering objects that
a real decrement already touched.

Scope: **`VAL_ARRAY`/`VAL_MAP` only.** Strings, numbers, bools, nil
can't be shared, so they can't cycle. Functions/closures use a
separate refcount in `chunk.c`'s `KhanClosure` and could in principle
cycle too (a closure capturing a variable that indirectly holds the
same closure) — much rarer in practice, and a documented follow-up,
not covered here.

## The algorithm, in one paragraph

Every time a decrement leaves an object's `ref_count` above zero (so
it wasn't freed the normal way), that object *might* now be part of an
unreachable cycle — it's added to a small "possible roots" buffer
instead of being examined immediately (checking on every single
decrement would be far too expensive). `gc_collect_cycles()` processes
that whole buffer at once, in three passes:

1. **MarkGray** — trial-decrement every object reachable from a
   possible root, as if that root's own references didn't count, so
   anything still "purely" cyclic ends up with a trial count of zero.
2. **Scan** — for each object touched, if its trial count is still
   positive it has a real external reference, and everything reachable
   from it is restored to black/live (undoing the trial decrements);
   anything left at zero is colored white — confirmed cycle garbage.
3. **CollectWhite** — actually free every white object.

Objects that survive (black) are completely unaffected — their real
`ref_count`s are exactly what they were before collection started.
That's what makes "trial" the right word.

Runs automatically every 4096 array/map allocations (`obj_new()`'s
counter in `value.c`), and is exposed to Kosh scripts as `gc_collect()`
for deterministic testing.

## Bug #1: reusing `color` as a free-phase reentrancy guard

First working version crashed with a **stack overflow** the moment it
was tested against a real self-referencing array (`a[0] = a`) —
`value_free` recursing into itself indefinitely.

`gc_collect_white()` needs to guard against visiting the same object
twice within one free pass (a self-reference or multi-object cycle
would otherwise revisit it and recurse forever). The first version
used `color` for that guard — flipping it to black on entry, the same
way `gc_scan_black()` legitimately uses black to mean "proven live."
That collided with the very check the function needs to make about
each child: "is this child *also* white (same garbage batch, free it
structurally) or is it black (has a real external reference, give it
a normal `value_free()` decrement instead)?" For a self-edge, flipping
the parent to black *before* checking its own child (which is itself)
made that self-edge look exactly like "escaped to a live object
outside the cycle" — so `value_free()` got called on the very object
still being torn down, which tried to free its own child (itself)
again, forever.

**Fix:** use `buffered` for the free-phase reentrancy guard instead,
leaving `color` untouched throughout `gc_collect_white()` so the
white/black check stays accurate for every remaining edge no matter
what order objects get freed in.

## Bug #2: a stale pointer in the roots buffer itself

Fixing #1 made the self-reference case work, but a **two-node cycle**
(`a[0] = b; b[0] = a;`) still crashed — a use-after-free this time, not
infinite recursion, and only reproducible with at least two roots in
the same collection batch where one's subtree reaches the other.

The `buffered` guard from Bug #1 only protects re-entry *through the
object graph* (following Value pointers between still-live Objs). It
does nothing for the **raw pointer sitting in `gc_collect_cycles()`'s
own `gc_roots` array**. With a two-node cycle, both `a` and `b` end up
in that array as separate root entries. Processing the first root
(`a`) recursively frees the second one too (`b`, reachable as just
another white node from `a`'s subtree) — and then `gc_collect_cycles`'s
driving loop, continuing on to its own next slot, calls
`gc_collect_white()` on `b`'s now-dangling pointer. Reading `b->color`
on freed memory is undefined behavior regardless of what value you'd
hope to find there — no color check can save you at that point,
because the object doesn't exist anymore.

**Fix:** mirrors what `value_free()` already does for the ordinary
(non-cycle) free path — `gc_unbuffer()` scans `gc_roots` for a matching
pointer and nulls it out. Now called from `gc_collect_white()` too,
right before every actual `free()`, so `gc_collect_cycles()`'s own
array never holds a stale pointer past the point where the object it
points to is freed — regardless of whether that free happened via the
driving loop directly or via a cascade from an earlier root in the
same batch.

Both bugs were found by testing directly against real cycles under
AddressSanitizer, not by inspection — the second one specifically only
shows up with **two or more roots in the same buffer that reach each
other**, which a single self-reference test can't exercise. Worth
remembering as a class of bug: a reentrancy guard on the *object graph*
isn't the same thing as a reentrancy guard on *every raw pointer that
refers to those objects* — a driving loop iterating its own separate
list of pointers needs its own invalidation story.

## What was verified (all under AddressSanitizer, plus a final
production-flag `-O2` build with no sanitizer)

- Self-reference (`a[0] = a`)
- Two-node cycle (`a[0]=b; b[0]=a`)
- Three-node cycle (`a→b→c→a`)
- Map self-cycle
- Mixed array/map cycle
- 500 independent cycles collected in one batch
- A cycle that also holds a reference to a still-live external
  object — confirmed the external object survives with a correctly
  decremented (not inflated, not prematurely freed) ref_count
- A long acyclic chain (100 nodes) — confirmed the collector never
  touches genuinely live, non-cyclic data
- The full existing test suite (`tests/suites/*.kh`,
  `test_map_assign.kh`, `test_json_db.kh`, all of `examples/*.kh`) —
  zero regressions

**Leak measurement**, not just crash-freedom: comparing
`AddressSanitizer`'s `LeakSanitizer` byte counts between a build with
this collector and one without, on the same workload (5,000
self-referencing-cycle iterations): **403,548 bytes leaked without the
collector → 3,581 bytes with it** — a >99% reduction, and every single
byte of that remainder traces back (per LeakSanitizer's own stack
traces) to pre-existing, already-documented parser/AST/compiler
bookkeeping that's never freed at program teardown — completely
unrelated to arrays, maps, or this collector. Re-verified against a
larger, more varied stress script (3-node cycles, map cycles, mixed
cycles, 500-cycle batches): zero leaked allocations traced to
`value.c` or any `gc_*` function.

## Known limitation

Functions/closures aren't covered (see "Why trial deletion" above) —
a closure cycle would still leak. Not observed as a real-world problem
in this codebase's own test suite or examples, but flagged honestly
rather than silently scoped out.
