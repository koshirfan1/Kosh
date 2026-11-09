# What's real, what's a known gap

[← Back to the README](../README.md)

Most from-scratch language projects are tempted to round up. This one tries hard not to — a prior version of the `vision` package shipped a `detect_faces()` that returned two hardcoded boxes regardless of input, and the fix was to rip it out and build real Haar-cascade detection, not to keep it and call it done. That history is worth being upfront about, both the mistake and the fix.

**Real, verified, not mocked:**
- The bytecode compiler + VM — the actual execution path for every script you run
- `vision`'s face detection — real Viola-Jones cascade detection, not hardcoded output
- `ocr` — a genuine native bridge to libtesseract; feeds it real pixel data, not a wrapper around the `tesseract` CLI. It is **opt-in** (`make OCR=1`, see [docs/ocr.md](ocr.md)); the default build does not include it. CI builds it and runs its suite on Linux
- `sqlite` — a real bridge to `libsqlite3` (`sqlite3_prepare_v2`/`sqlite3_bind_*`/`sqlite3_step`), not a JSON-backed mock (an earlier version was; ripped out and rebuilt for real)
- `{}` maps — a real open-addressing hash index (FNV-1a), not a linear scan; the old O(n) version made a 50,000-key map take ~9 seconds, the current one takes ~0.01s (~725x). Full story in [docs/hash-table-audit.md](hash-table-audit.md)
- `try`/`catch`/`throw` — a real exception-handling implementation (unwinds the VM stack and call frames to the nearest handler), not just a parser accepting the syntax. Catches both user `throw`s and every built-in runtime error. Design and a real bug found along the way in [docs/trycatch-implementation.md](trycatch-implementation.md)
- `class` — Python-style classes: `fn` methods with an explicit `self`, `__init__` constructors, dynamic fields, method chaining, single inheritance (`class Dog(Animal):`) with `super`, all working with `try`/`catch`. No multiple inheritance or bound-method values yet. See [docs/classes.md](classes.md)
- `Tensor` — a real native tensor type (`VAL_TENSOR`: one contiguous buffer, not nested arrays) with automatic differentiation: `add`/`sub`/`mul`/`matmul`/`transpose`/`reshape`/`sum`/`relu`/`sigmoid`/`tanh`, `.backward()`, `.grad`, row/column broadcasting (e.g. a shared bias across a batch). The first slice of Kosh's AI-native-core direction. 2-D-only matmul, no GPU. See [docs/tensor.md](tensor.md)
- A cycle collector for circular references in arrays/maps (`a[0] = a` no longer leaks for the life of the process) — a Bacon-Rajan trial-deletion collector, the same algorithm CPython's `gc` module uses, not a full tracing GC. Two real bugs found and fixed during testing, plus a >99% leak reduction measured (not just "doesn't crash") in [docs/gc-notes.md](gc-notes.md)
- Testing: thousands of fuzz-mutated parser inputs with zero crashes, memory stress-tested at 10M-allocation scale with flat RSS. CI (Linux and Windows; the macOS steps exist but macOS is not in the build matrix yet) runs the assertion suites and parser fuzzing, plus an AddressSanitizer/UBSan job on Linux. The Webi encrypted channel has deterministic property fuzzers and differential tests against OpenSSL ([docs/webi-testing.md](webi-testing.md))

**Known, open gaps — not hidden, not fixed yet:**
- String concatenation in a loop is **partially** fixed — a redundant
  length-rescan on the growing accumulator was removed (a real ~2.4x
  win, verified), but the deeper cause is still open:
  `OP_SET_LOCAL`/`OP_SET_GLOBAL`/`OP_SET_UPVALUE` deep-copy the
  assigned value on every write by design, so building a string with
  repeated `+=` in a loop is still worse than linear overall. Full
  story in [benchmarks/RESULTS.md](../benchmarks/RESULTS.md)'s
  `string_concat` section.
- Closures are value-capture only — a nested function can read an
  enclosing variable, but there's no shared mutable upvalue, so a
  "counter closure" pattern that mutates captured state doesn't work
  the way it does in Python/JS.

If a claim in this README turns out to not match reality, that's a bug in the README — [open an issue](https://github.com/koshirfan1/Kosh/issues).

**On performance, specifically**: Kosh is generally slower than Python and Node right now, not faster — roughly 2-8x slower on tight loops, competitive only on JSON round-trips (where it's actually ~2.4x faster than Python). Computed-goto dispatch was tried as the obvious next lever and **measured slower, not faster**, than the existing switch-based dispatch (5-15% depending on benchmark) — reverted; full measurement and why in [docs/dispatch-perf.md](dispatch-perf.md). The next real lead is a `value_copy()` call on every basic operation (even a plain number) — not yet profiled, flagged as the most promising thing to actually measure next. Full numbers and methodology in [benchmarks/RESULTS.md](../benchmarks/RESULTS.md).
- The Webi encrypted channel (`webi` 1.2.0, opt-in) has **not had an independent cryptographic or protocol review**. It runs on top of TLS and is documented in [webi-threat-model.md](webi-threat-model.md).
- `kh install` verifies every downloaded file against a SHA-256 from the registry and installs dependencies first. It cannot defend against a compromised repository, because the registry and the packages live in the same one (a signed registry would be needed).
- Not yet exercised in real browsers (Chrome, Firefox, Safari) or on Windows for the new Webi pieces.
