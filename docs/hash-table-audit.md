# Hash Table / Map Audit

Roadmap item #11 assumed Kosh's dictionary/map type is backed by a hash
table and asked for its collision handling, resize policy, and O(1)
average performance to be checked. The actual finding is more
fundamental than that: **there are two completely different, unrelated
data structures in this codebase that both happen to be called
"table"/"map", and only one of them is actually a hash table.**

## 1. `Table` (`src/vm.c`) — a real hash table, used only for globals

This is what backs the VM's global-variable lookup (`OP_GET_GLOBAL`/
`OP_SET_GLOBAL`/`OP_DEF_GLOBAL` — see `docs/opcodes.md`). It's a
textbook, solid implementation:

- **Hash function**: FNV-1a over the key string (`hash_string()`) — a
  well-regarded, low-collision choice for short string keys like
  variable/function names.
- **Collision handling**: open addressing with linear probing
  (`find_entry()` — `index = (index + 1) % capacity` on collision).
- **Resize policy**: grows (doubles, starting at capacity 8) once load
  factor would exceed 75% (`table_set`'s `count + 1 > capacity * 0.75`
  check) — a standard, reasonable threshold.
- **No deletion path exists** (no `table_delete`), which also means no
  tombstone handling was needed and none exists. This is consistent
  with the language not exposing any way to "undeclare" a global, so
  this is very likely a non-issue rather than an oversight — flagging
  only so it's a documented, deliberate observation rather than a
  silent gap.

This part of the audit is genuinely ✅ done: it's a correct, standard
hash table implementation. No changes recommended here.

## 2. The Kosh-language map type (`{}` literals) — NOT a hash table

This is the one that actually matters for real Kosh programs — every
`{}` literal, every `m["key"]` read/write, every `has()`/`keys()` call
goes through this. `map_set()`/`map_get()` in `src/value.c` are a
**linear-scan array**, not a hash table at all:

```c
void map_set(Value *map, const char *key, Value value) {
    ...
    for (int i = 0; i < o->as.map.count; i++) {          // <- O(n) scan
        if (strcmp(o->as.map.entries[i].key, key) == 0) {
            ...
        }
    }
    // ... append if not found (realloc-doubling growth, which part is fine)
}

Value *map_get(Value *map, const char *key) {
    for (int i = 0; i < o->as.map.count; i++) {          // <- O(n) scan
        if (strcmp(o->as.map.entries[i].key, key) == 0) {
            return o->as.map.entries[i].value;
        }
    }
    return NULL;
}
```

Every `map_set` on an existing key, and every `map_get`, does a full
linear scan of every key already in the map. Building up a map with N
keys one at a time is therefore O(N²) — verified empirically, not just
theoretically:

| keys inserted | wall time |
|---|---|
| 1,000 | 0.005s |
| 4,000 (4x) | 0.033s (~7x) |
| 8,000 (2x again) | 0.098s (~3x) |

That's clear super-linear growth consistent with O(N²) (small-N noise
means the ratios aren't textbook-exact, but the trend is unambiguous —
linear growth would show ~4x and ~2x respectively, not ~7x and ~3x).
Reproduce with:
```sh
# build tests/_map_scale_N.kh with a while loop inserting N keys, time it
```

## Why this wasn't fixed here

This is a real performance bug with an obvious, common trigger — any
code that builds a map with many keys (deduplication, grouping,
building a lookup table/index, memoization) degrades quadratically. But
fixing it means replacing the map's underlying representation with an
actual hash table, which:

- Changes `MapEntry`/`Obj`'s map variant in `src/interpreter.h`
- Touches every direct reader of `.as.map.entries`/`.as.map.count`
  across `value.c`, `vm.c`, `vision_lib.c`/`vision_cv.c` (which build
  map values directly in C — see the `vision` package's native layer),
  `json_lib.c`, and likely more
- Has a real semantic question to resolve first: the current
  linear-array implementation happens to **preserve insertion order**
  when iterating (`keys()`, `json_encode()`, printing a map) — a real
  hash table with open addressing does not, unless deliberately
  designed to (e.g. maintaining a separate insertion-order side array,
  like Python's dict since 3.7). If any existing Kosh code or package
  relies on map iteration order (worth checking before touching this),
  the replacement needs to preserve it deliberately, not lose it as a
  side effect.

This is a real, scoped, well-understood next step — not attempted here
because it's a Value-system-wide change (roadmap items #9/#11
together) that deserves its own dedicated pass with its own careful
regression testing, not something to fold into a benchmarking/audit
session. Flagging it as the highest-value concrete finding from this
audit, same spirit as the string-concatenation finding in
`benchmarks/RESULTS.md` (which, incidentally, shares the same root
category of problem: something that's O(n) per operation being called
in a loop that makes the whole thing O(n²)).

## Recommendation for whoever picks this up

Two independent choices, not mutually exclusive:
1. Replace `map_set`/`map_get`'s linear scan with real hashing
   (reuse `hash_string()`'s FNV-1a from `vm.c`, or promote it to a
   shared header) while preserving insertion-order iteration via a
   side array or an intrusive linked list through entries.
2. At minimum, benchmark whether typical Kosh programs actually build
   large maps (if most real usage is <20 keys, this may be lower
   priority than it looks in a synthetic worst-case benchmark) —
   `benchmarks/run.py` doesn't currently include a map-scaling
   benchmark; adding one there would make this an ongoing, trackable
   metric instead of a one-time finding.

## Resolved

Option 1 above is now implemented. `interpreter.h`'s map struct
gained a side `hash_index`/`hash_capacity` (open-addressing,
FNV-1a, linear probing, doubles at ~70% load) that indexes into the
existing `entries[]` dense array. `entries[]` itself is untouched —
still populated and read in strict insertion order — so every one of
the 20+ direct callers of `map_set`/`map_get`, plus every direct
`AS_MAP_ENTRIES`/`AS_MAP_COUNT` reader (`json_lib.c`, `requests_lib.c`,
`webi_lib.c`, `khan_stdlib.c`'s `keys()`), needed zero changes.
`map_set`/`map_get`'s public signatures are identical.

No deletion path exists anywhere for maps (checked across the whole
codebase before implementing), so no tombstones were needed — every
hash-index slot is either empty (`-1`) or a valid `entries[]` index.

Measured: `benchmarks/map_scale.kh` (4,000 keys) went from ~0.031s to
~0.005s (~6x). A synthetic 50,000-key insert+lookup benchmark — closer
to where the old O(n²) behavior actually hurts — went from ~8.99s to
~0.012s (~725x), confirming the win compounds with scale exactly as
the quadratic-vs-linear analysis above predicted. Full test suites
(`tests/suites/*.kh`, `test_map_assign.kh`, `test_json_db.kh`,
`examples/maps_test.kh`) all still pass, including insertion-order-
dependent output (`keys()`, JSON encoding).
