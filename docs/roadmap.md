# Roadmap

[← Back to the README](../README.md)

| Phase | Status |
|---|---|
| Lexer | ✅ Complete |
| Parser | ✅ Complete |
| Tree-walk interpreter (original design) | ✅ Complete, now legacy — superseded by the compiler + VM below |
| Bytecode compiler + VM | ✅ Complete — the actual execution engine (38 opcodes, see [docs/opcodes.md](opcodes.md)) |
| Standard Library | ✅ Complete |
| Import/Module system, `from X import A, B, C` | ✅ Complete |
| Package manager (`kh`), 38-package registry | ✅ Complete |
| `webi` — routing, security (CSRF/rate-limit/API-key/CORS), templates, static files, `after()` hooks | ✅ Complete |
| `vision` — image I/O, filters, thresholding, morphology, real Haar-cascade face detection | ✅ Complete |
| `ocr` — Tesseract bridge: text, word boxes, orientation correction, searchable PDF, whitelisting, multi-language | ✅ Complete — opt-in build (`make OCR=1`), tested in CI on Linux; see [docs/ocr.md](ocr.md) |
| Fuzz testing, memory stress testing (10M-allocation scale), CI | ✅ Complete for what exists — CI runs the full assertion suites on Linux and Windows (macOS steps exist, macOS is not in the matrix yet), parser mutation fuzzing, and an AddressSanitizer/UBSan job on Linux |
| Security fixes: curl command buffer overflow, `kh` input validation, rate limiter fail-open, `crypto_lib` linked | ✅ Complete in v0.016.0, see [CHANGELOG.md](../CHANGELOG.md) |
| `webi` encrypted channel (X25519 + HKDF + ChaCha20-Poly1305, replay window), `require_secure`, crawler verification, browser client, `/_webi/proof` | ✅ Implemented and fuzzed in v0.016.0 — **no external review yet**, see [webi-threat-model.md](webi-threat-model.md) |
| `kh` per-file SHA-256 verification, staged atomic installs, dependency installation | ✅ Complete in v0.016.0, see [packages.md](packages.md#integrity-and-dependencies) |
| Signed package registry | 🔲 Planned |
| macOS in the CI matrix | 🔲 Planned |
| `{}` map as a real hash table | ✅ Complete — open-addressing (FNV-1a), see [docs/hash-table-audit.md](hash-table-audit.md) |
| String concatenation performance | 🟡 Partial — redundant rescan fixed (~2.4x), full O(n²) root cause still open, see [benchmarks/RESULTS.md](../benchmarks/RESULTS.md) |
| Mutable closures (currently value-capture only) | 🔲 Planned |
| Garbage collector (circular references) | ✅ Complete — cycle collector for arrays/maps, see [docs/gc-notes.md](gc-notes.md) |
| Error handling (`try`/`catch`/`throw`) | ✅ Complete, see [docs/trycatch-implementation.md](trycatch-implementation.md) |
| Classes (`class`, `self`, `__init__`, methods, fields, inheritance, `super`) | ✅ v1 complete (single inheritance only), see [docs/classes.md](classes.md) |
| Tensor + autograd (`Tensor`, `.backward()`, `.grad`, row/col broadcasting) | ✅ v1 complete (2-D-only matmul/transpose), see [docs/tensor.md](tensor.md) |
| nn module (`Dense`, activations, `Sequential`, MSE/CrossEntropy loss, `SGD`/`Adam`) | ✅ v1 complete, see [docs/nn.md](nn.md) |
| Dispatch-loop optimization (computed goto) | ✅ Tried, measured slower than the existing switch, reverted — see [docs/dispatch-perf.md](dispatch-perf.md) |
| `webi` threaded server (thread-per-connection, concurrency cap) — see [docs/phase4-plan.md](phase4-plan.md) | 🔲 Planned |
| ONNX Runtime bridge (run pretrained deep-learning OCR models) — no design doc yet | 🔲 Planned |
| Binary-safe string type (length-prefixed, not NUL-terminated) | 🔲 Planned |
| Self-hosted compiler | 🔲 Planned |
| **v1.0 release** | 🔲 Not yet — see `ROADMAP_STATUS_UPDATED.md` for the full, current criteria-by-criteria status |
