# Phase 3 plan — AI Operating Platform (v2.0)

Status: **planning input, not yet scoped into an implementation plan.**
Same treatment as `docs/phase2-ai-foundation-plan.md`: Part 1 records the
proposal as given, translated and organized; Part 2 checks it against
the actual codebase. Nothing here is built. Phase 3 is written to build
on Phase 2, and Phase 2 itself is still unbuilt (see that doc's Part 2) —
worth keeping in mind throughout: most of what follows is gated on Phase
2 landing first, on top of whatever Phase 3 needs beyond that.

**Phase goal, as given:** a language in which models can be trained,
deployed, and turned into production AI systems.

---

## Part 1 — The proposal, as given

### 1. Distributed Training ⭐⭐⭐⭐⭐

Not limited to one machine.

```
trainer = DistributedTrainer(
    devices = 8,
    backend = "nccl"
)

trainer.train(model)
```

Features: Multi-GPU, Multi-Machine, Data Parallel, Model Parallel,
Pipeline Parallel.

### 2. AI Compiler

Optimize tensor operations — e.g. compile `y = (a+b)*c` into a single
fused kernel instead of separate `ADD` / `STORE` / `LOAD` / `MUL` steps.
A significant performance win.

### 3. JIT Compiler ⭐⭐⭐⭐⭐

Today: a bytecode VM. Future:

```
Source
  ↓
Bytecode
  ↓
Hot Function
  ↓
Native Machine Code
```

Loop-heavy code should automatically become native — the goal is
performance meaningfully ahead of Python.

### 4. AI Server

A built-in AI server.

```
server = ai.server()
server.load("llama.gguf")
server.run(8000)
```

Automatic: REST API, Streaming, Chat API, Embeddings.

### 5. Agent Framework

```
agent = Agent()
agent.tool(web)
agent.tool(sqlite)
agent.tool(shell)
agent.chat()
```

Called out as very useful for future AI projects.

### 6. Vector Database

Built-in `Vector()`, `Similarity()`, `Cosine()`, `Search()`.

```
db.insert(vector)
db.search(query)
```

### 7. RAG Framework

```
rag = RAG()
rag.add("./docs")
rag.ask("Explain VM")
```

Automatically: Chunks → Embedding → Vector Search → LLM.

### 8. AI Networking

Special protocols for Model Download, Streaming, Inference, Remote
Execution.

### 9. Security ⭐⭐⭐⭐⭐

AI-specific security: Encrypted Models, Encrypted Weights, Model
Signing, License Verification, Sandbox, Permission System.

### 10. Plugin System

```
plugins/
  vision
  audio
  rag
  robotics
  cuda
```

Dynamic loading.

### 11. Robotics (future)

```
camera.read()
motor.forward()
sensor.read()
```

AI + robotics.

### 12. AI Debugger

Debug Tensor Shapes, Memory Usage, GPU Usage, Gradients, NaN Detection.

### 13. AI Profiler

Timing visibility across Training → Forward → Backward → Optimizer.

### 14. Production Deployment

One command:

```
kh deploy app.kh
```

Automatically: Docker → HTTP Server → TLS → Load Balancer.

### 15. AI Package Registry

```
kh install yolo
kh install llama
kh install whisper
```

### 16. Cross Platform

Linux, Windows, macOS, Android, Raspberry Pi, ARM.

### 17. Long-term VM Evolution

Current:

```
Source → Compiler → Bytecode → VM
```

Future:

```
Source → Compiler → Bytecode → VM → JIT → Machine Code
```

### 18. Enterprise Features

Hot Reload, Memory Snapshot, Crash Recovery, Remote Debugging,
Profiling, Monitoring.

### Positioning recommendation, as given

> After reviewing the repository, Kosh's biggest strength can be
> AI + Networking + Security. So for Phase 3, no time goes into a GUI
> framework, game engine, mobile toolkit, or browser.

Proposed positioning: *"A high-performance systems language for AI
infrastructure, secure networking, and intelligent services."*

### The overall 3-phase roadmap, as given

- ✅ **Phase 1 (v1.0):** Core Language, Compiler, Bytecode VM, Standard
  Library, Testing, Documentation.
- ✅ **Phase 2 (v1.5):** Tensor Engine, Autograd, Neural Networks, AI
  Runtime, Vision, Model Training, GPU Support.
- ✅ **Phase 3 (v2.0):** Distributed AI, JIT Compiler, AI Server, AI
  Agents, RAG, Vector Database, Production Deployment, Enterprise
  Tooling.

### One more suggestion, as given

> After Phase 3, dedicate 3–6 months purely to optimization and
> stability instead of new features. Languages like Go, Rust, and Zig
> matured the way they did because every release prioritized
> reliability and performance, not just features — that's what makes
> Kosh stronger long-term.

---

## Part 2 — Reality check against the current codebase

Same approach as the Phase 2 doc: checked directly against `src/` and
`packages/`, not assumed. Headline: **every Phase 3 section is gated on
Phase 2 landing first** (there's no tensor/autograd/NN/GPU layer to
train, serve, secure, or debug yet), and on top of that gate, three of
Phase 3's own sections have zero foundation of their own today — no
JIT, no dynamic-loading/plugin mechanism, no crypto primitives. A few
sections, though, have more of a running start than they might look
like at first glance.

### §15 AI Package Registry — already real, and the best-positioned item in this whole phase

`kh` (`src/kh.c`) is not a stub — it's a working package manager today:
`kh install <name>`, `kh remove <name>`, `kh list`, `kh installed`,
`kh info <name>`, all backed by a real remote registry fetch
(`download_to_file()` pulls from
`raw.githubusercontent.com/khandev1211-cpu/Khan/main/packages/...`,
confirmed directly in `src/kh.c`). `kh install yolo` / `kh install
llama` / `kh install whisper` need nothing new in `kh` itself — they
need `yolo`/`llama`/`whisper` packages to exist and get registry
entries in `packages/registry.json`, the same way every existing
package already does. This is the one Phase 3 section that's a content
problem, not an engineering one.

### §4 AI Server has a real transport layer to build on — with a known, already-documented gap

`packages/webi` is a genuine HTTP framework (routing, middleware,
JSON/streaming-shaped responses, the works — see `docs/webi.md`), so
`server.run(8000)` doesn't need a new HTTP stack from scratch. But
`docs/phase4-plan.md` (already in this repo) independently found that
`webi`'s server is single-connection-at-a-time today — one slow request
blocks every other visitor — and that fix is itself still "planning
only, not yet implemented." An AI server needs to hold open streaming
responses (token-by-token chat output) while still accepting new
connections, which is *exactly* the scenario Phase 4's plan is written
to fix. In other words: §4 doesn't just depend on Phase 2 (needs
something to load/run — `llama.gguf` loading doesn't exist yet either,
see Phase 2 doc §11), it also depends on Phase 4's still-unbuilt
concurrency work, or streaming responses will block the whole server
per request.

### §3 / §17 JIT Compiler — accurately described as future work; today's VM is the simplest possible baseline

Confirmed in `src/vm.c`: dispatch is a plain C `switch` statement over
the opcode (`run_loop()`, line ~203) — no computed-goto/threaded
dispatch (already flagged as unmeasured in Phase 1's roadmap item #6),
let alone a JIT. Going from here to "hot function → native machine
code" is not an incremental step — it's typically one of the largest
subsystems in any language runtime (LuaJIT, V8's TurboFan, PyPy's
tracing JIT are each multi-year projects on their own). Sequencing this
after distributed training/AI compiler (as Part 1 already does, §3
coming before the compiler-optimization work in §2 conceptually
depends on) seems right; flagging mainly so the scale of §3 isn't
underestimated relative to everything else on this list.

### §10 Plugin System — blocked by the same gap the Phase 2 doc already found

The Phase 2 doc's Part 2 already confirmed there is no `dlopen`/FFI/
dynamic-loading code anywhere in `src/` — every capability today is a
`.c` file statically linked into the `kosh` binary via the `makefile`.
A `plugins/` directory with dynamically-loaded `vision`/`audio`/`rag`/
`robotics`/`cuda` modules needs that mechanism built first (real
`dlopen`/`LoadLibrary` plumbing, a stable C ABI for plugins to target,
versioning/compatibility handling) — this is new infrastructure, not a
folder convention on top of what exists.

### §9 Security — no cryptographic primitives exist to build any of this on

Checked directly: no SHA-256/AES/HMAC/OpenSSL integration anywhere in
`src/`. The only crypto-adjacent code found is a cryptographically
random hex-token generator in `webi_lib.c` (used for CSRF tokens, per
`docs/phase5-hardening-and-design-plan.md`'s security work) — nowhere
near "Encrypted Models," "Model Signing," or "License Verification,"
all of which need real hashing/signing/encryption primitives (almost
certainly via a native crypto library binding, e.g. OpenSSL or a
smaller alternative like mbedTLS/libsodium) that don't exist yet.
Related and worth flagging in the same breath: `docs/webi.md` and
`webi_lib.c` show **no TLS/HTTPS support at all today** — the whole
server is plain HTTP — which also affects §14's "Docker → HTTP Server →
TLS → Load Balancer" pipeline; TLS would need to either terminate
outside Kosh entirely (a reverse proxy in front of `webi`, the more
common and lower-risk pattern) or get built into `webi_lib.c` directly.

### §5 Agent Framework — `agent.tool(shell)` is a real security decision, not just a missing function

`sqlite` (tool target #2) already exists and works (smoke-tested in the
correctness session — see `ROADMAP_STATUS_UPDATED.md`). `web`
(presumably HTTP fetch) maps onto the existing `requests` package.
`shell`, however, has no equivalent today — there is no general-purpose
"run an arbitrary shell command" native function exposed to Kosh
scripts (the only `popen()` use in the codebase is internal, inside
`requests_lib.c`, to shell out to `curl`, and it's not exposed as a
capability Kosh code can call directly). Exposing arbitrary shell
execution to an agent that's also taking natural-language instructions
from an LLM is a real prompt-injection/RCE surface — this probably
wants to land *after*, and be gated by, whatever comes out of §9
Security's sandbox/permission-system work, not before it. Building
`agent.tool(shell)` first and `Sandbox`/`Permission System` later
would leave a real gap open in between.

### §1 Distributed Training — the gate is Phase 2, not concurrency, worth being precise about

It's tempting to read "distributed" and assume Kosh's interpreter needs
to be made thread-safe first — but the standard pattern real frameworks
use (PyTorch DDP, Horovod) is one OS process per GPU/machine,
coordinated over the network via something NCCL-equivalent, not one
multi-threaded interpreter juggling every device. Under that model,
Kosh's interpreter staying single-threaded per-process isn't
disqualifying. What *is* missing: any process-spawning/IPC
infrastructure at all (already confirmed absent when `docs/
phase4-plan.md` evaluated a process-pool option for the webi server —
"no fork()-equivalent abstraction exists in this codebase yet"), a
network-collective-communication binding (an NCCL/MPI-equivalent), and,
underneath all of it, the actual tensor/autograd/GPU layer from Phase 2
to distribute in the first place. So §1 is gated primarily by Phase 2,
secondarily by new IPC/networking infrastructure — not by anything in
the interpreter's threading model.

### §2 AI Compiler (kernel fusion) — depends on two things, neither built

Needs (a) Phase 1's own compiler-optimization work (`ROADMAP_STATUS_UPDATED.md`
item #4 — constant folding, dead code elimination, peephole
optimization — currently ❌ not started at all), and (b) some kind of
tensor operation graph/IR to fuse across, which can't exist before
Phase 2's tensor engine does. Both prerequisites, not just one.

### §16 Cross Platform — partially true today, unverified everywhere else

The codebase already has a real POSIX/Windows split (`#ifdef _WIN32`
throughout `main.c`, `webi_lib.c`, and elsewhere) and both `kosh.exe`/
`kh.exe` (Windows) and the Linux binaries built from the same source
ship in this repo. macOS is POSIX-compliant so the existing code
*likely* builds there too, but this has not been verified — there's no
CI (Phase 1 roadmap item #17), so nothing has actually confirmed a
macOS build, let alone Android/Raspberry Pi/ARM. Those last three
aren't just "recompile and go," either: Android specifically needs an
NDK toolchain and almost certainly a different I/O/permissions model
than desktop POSIX assumes; Raspberry Pi/ARM are more likely to "just
work" as a plain recompile (it's standard C) but that's an assumption,
not a confirmed fact, until someone actually builds and runs the test
suite there.

### §6/§7/§11/§12/§13/§18 — no existing code, correctly scoped as new work

Checked and confirmed absent, same as most of Phase 2's later sections:
no vector database/similarity search, no RAG pipeline, no hardware I/O
(camera/motor/sensor) bindings, no tensor-shape/gradient/GPU debugger,
no training-loop profiler, no hot-reload/memory-snapshot/crash-recovery/
remote-debugging/monitoring infrastructure. These are all genuinely
greenfield and don't have a "here's the closest existing thing" the way
§4/§15 do.

### Positioning recommendation — consistent with what's actually in the repo

The "AI + Networking + Security" positioning lines up with what already
exists and works today (a real HTTP framework in `webi`, a real package
manager in `kh`, a real — if basic — CV toolkit in `vision`) far better
than a GUI/game-engine/browser direction would; there's no windowing,
rendering, or graphics code anywhere in this codebase to build any of
those on top of. Skipping them isn't just a focus call, it avoids
starting three more greenfield subsystems on top of the ones Phase 2
and Phase 3 already require.

### On the "3–6 months of optimization and stability" suggestion

Directly consistent with where Phase 1 already stands per
`ROADMAP_STATUS_UPDATED.md`: performance benchmarking (#16), CI (#17),
and full documentation (#18) are all still ❌ not started *today*, before
any of Phase 2 or 3 has been built. Every phase above adds substantially
more surface area (a JIT, distributed training, a plugin/dynamic-loading
system, real crypto) that will need the same rigor applied to it that
Phase 1's own correctness passes just went through — each of which found
real, previously-unnoticed bugs (a build that didn't compile on Linux at
all, a closures gap, a dead security feature) purely by finally running
everything and checking it, not by guessing where problems might be.
The suggestion to protect dedicated stabilization time after Phase 3,
rather than only after Phase 1, is worth taking seriously for exactly
that reason.
