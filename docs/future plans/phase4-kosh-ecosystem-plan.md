# Phase 4 plan — Kosh Ecosystem (v3.0 vision)

Status: **planning input, not yet scoped into an implementation plan.**
Same treatment as `docs/phase2-ai-foundation-plan.md` and `docs/
phase3-ai-operating-platform-plan.md`: Part 1 records the proposal as
given, translated and organized; Part 2 checks it against the actual
codebase.

**Naming note:** this repo already has a `docs/phase4-plan.md` — an
unrelated, earlier "Phase 4" about making the `webi` HTTP server
handle connections concurrently (thread-per-connection + a dispatch
lock). That's a different numbering track (webi-specific sub-phases)
from this one (the brother's overall 4-phase language roadmap:
Core → AI Foundation → AI Operating Platform → Ecosystem). Keeping this
document under a distinct filename (`phase4-kosh-ecosystem-plan.md`)
deliberately to avoid a collision — worth renaming one of the two
tracks later so "Phase 4" means one thing consistently across `docs/`.

**Phase goal, as given:** this is presented as the point where Kosh
stops being "a language with AI features" and becomes a full
professional ecosystem — its own tooling, editor support, security
identity, and distribution system — with feature growth intentionally
ending here. Explicitly: *"after Phase 4, stop adding new features
almost entirely — from then on, only performance, security, bug fixes,
documentation, tests, and stable releases."*

---

## Part 1 — The proposal, as given

### 1. Self-Hosting ⭐⭐⭐⭐⭐

Called out as the first target. Kosh's own compiler and package
manager gradually get rewritten *in Kosh itself*.

```
Today:   C    → Compiler
Future:  Kosh → Compiler
```

Framed as a milestone every mature language reaches.

### 2. Kosh IDE

A dedicated editor. Features: AI Completion, Refactoring, Debugger,
Memory Viewer, VM Inspector, AI Chat, Profiler.

```
F5 → Compile → Run → Profile
```

### 3. Native Package Registry

The Python/Rust/Go equivalents: `pip`, `cargo`, `go get`. Kosh's
version:

```
kh install webi
kh install vision
kh install ai
```

Plus publishing: `kh publish`.

### 4. Documentation Generator

```
kh doc
```

Automatically produces HTML, PDF, Markdown, and API docs.

### 5. AI Compiler Assistant

The compiler proactively suggests improvements:

```
Warning → "This loop can be vectorized." → Apply?
```

or

```
Unused Variable → Remove?
```

### 6. Memory Analyzer

```
Memory → Heap → Objects → Leaks → Reference Graph
```

Meant to make debugging easier.

### 7. Static Analyzer

Compile-time detection of: Null Dereference, Unused Variable, Dead
Code, Race Condition, Integer Overflow.

### 8. Security Scanner ⭐⭐⭐⭐⭐

Called out as what should become Kosh's identity.

```
kh audit
```

Output: Weak Hash, SQL Injection, Path Traversal, Command Injection,
Unsafe Random. Explicitly: this should be built-in, not optional.

### 9. AI Code Review

```
kh review
```

Automatically covers: Performance, Security, Memory, Complexity,
Suggestions.

### 10. Native Build System

```
kh build
```

Automatically targets Windows, Linux, Mac, Android.

### 11. Cross Compilation

```
kh build --target windows
kh build --target linux
kh build --target arm64
```

### 12. Native Installer

```
kh package
```

Output: `.exe`, `.deb`, `.apk`, `.rpm`, `.pkg`.

### 13. Language Server

One implementation serving VS Code, Neovim, and JetBrains. Features:
Auto Complete, Go To Definition, Rename, Hover, Diagnostics.

### 14. AI Documentation

```
kh explain parser.kh
```

Automatically produces: Architecture, Functions, Call Graph,
Explanation.

### 15. VM Profiler

```
kh profile app.kh
```

Output: Opcode Count, Function Calls, Heap Usage, Execution Time,
Native Calls.

### 16. Native Testing Framework

```
kh test
```

Automatically covers: Unit, Integration, Performance, Memory,
Regression.

### 17. Enterprise Security

Encrypted Bytecode, Code Signing, Sandbox, Permission System,
Capability-Based APIs.

### 18. Plugin Marketplace

```
kh install mqtt
kh install robotics
kh install tensorflow
kh install cuda
```

### 19. Complete AI Ecosystem

The entire lifecycle in one language:

```
AI → Training → Evaluation → Deployment → Monitoring → Updating → Inference
```

### 20. Self-Optimizing Compiler ⭐⭐⭐⭐⭐

Described as the dream feature. The compiler profiles itself
automatically:

```
Loop → Hot → Optimize → Native Code → Cache
```

No user intervention required.

### Kosh v3.0 vision, as given

```
                Kosh
                  │
          ┌───────┼────────┐
          │       │        │
       AI Core  Security  Networking
          │       │        │
          └───────┼────────┘
                  │
            Bytecode VM
                  │
            Native Runtime
                  │
      Linux / Windows / Android
                  │
              GPU / CPU
```

### Closing framing, as given

> If this were my project, I'd almost completely stop adding new
> features after Phase 4. After that, focus only on: 🚀 Performance,
> 🔒 Security, 🐛 Bug fixes, 📚 Documentation, 🧪 Tests, 📦 Stable releases.

---

## Part 2 — Reality check against the current codebase

Same approach as the Phase 2 and Phase 3 docs. Headline: **Phase 4 is
almost entirely tooling built *around* the language (compiler-as-a-
service, IDE, package registry, build system) rather than language
features**, which is a different kind of work from Phases 1–3 — closer
to `docs/phase5-hardening-and-design-plan.md`'s security/infrastructure
work than to adding tensors or a JIT. A handful of items have a real
head start; most are genuinely greenfield; two (self-hosting, the
security scanner) are worth flagging as harder or easier than they
might look, in opposite directions.

### §3 Native Package Registry — already the most-built item across all three phase docs

Same finding as Phase 3 §15: `kh` (`src/kh.c`) already does `install`/
`remove`/`list`/`installed`/`info` against a real remote registry
(`raw.githubusercontent.com/koshirfan1/Kosh/main/packages/...`).
What's missing specifically for *this* section is `kh publish` —
confirmed absent (checked every `strcmp(cmd, ...)` branch in `src/
kh.c`; only the five commands above exist). Publishing needs the
reverse direction of what's built today (upload + registry-entry
creation + auth, not just download) — real new work, but sitting on a
solid, already-proven base.

### §16 Native Testing Framework — a real package to build `kh test` on top of, but no CLI wiring yet

`packages/test/test.kh` already exists and is exactly the assertion
library this session's correctness work leaned on throughout
(`test_suite`, `test_eq`, `test_gt`, `test_type`, `test_run`, etc. —
see `ROADMAP_STATUS_UPDATED.md`). What doesn't exist: `kh test` as a
CLI command that *discovers* test files, runs them, and aggregates
results — today every test file (`tests/test_*.kh`) has to be invoked
individually (`kosh tests/test_foo.kh`), and `tests/run_all.kh` is
itself a hand-written Kosh script that imports and calls each suite
directly, not a real test-runner. Turning that into `kh test` is a
CLI/discovery problem on top of an already-working assertion library —
one of the smaller lifts in this document.

### §1 Self-Hosting — the language has real gaps that would need filling first, beyond just "write more Kosh code"

Two concrete blockers, checked directly:

- **No bitwise operators at all.** Confirmed absent from `src/lexer.c`
  — no `&`, `|`, `^`, or shift tokens. A compiler/VM written in Kosh
  would need to manipulate raw bytecode bytes, opcodes, and flags — the
  kind of work that leans on bitwise operations constantly (the current
  *C* implementation of the VM does, e.g. opcode encoding, flag checks).
  This is a new-operator gap in the same family as the missing `@`
  operator already flagged in the Phase 2 doc.
- **No classes/structs, still.** Also already flagged in Phase 2's Part
  2: a self-hosted compiler is itself a natural fit for structured data
  (a `Token` type, a `Chunk`/bytecode-buffer type, an `AstNode` type) —
  today that would all have to be maps-plus-free-functions, which is
  workable (it's how the rest of the stdlib is built) but a real style
  and ergonomics tax on a project this size.
- Beyond language gaps: performance is a real open question. The
  current VM has no JIT (Phase 3 §3) and dispatches via a plain
  `switch` statement (confirmed in `src/vm.c`) — a Kosh-in-Kosh
  compiler running *on* that VM, compiling *other* Kosh programs, adds
  a real interpretation-overhead layer that today's fast, native-C
  compiler doesn't pay. Self-hosting typically becomes practical once a
  language is fast enough that the self-hosted compiler compiling
  itself isn't painfully slow — which likely means this item wants to
  come *after*, not before or alongside, Phase 3's JIT work, even
  though it's listed first here.

None of this means self-hosting is a bad first target — it's a
genuinely meaningful milestone, as described — just that "first" here
probably means "first to *plan*," not "first to *build*," given what
it actually depends on.

### §8 Security Scanner — has more to check against than it might seem, precisely because of what this session already found

`kh audit`'s target list (SQL Injection, Path Traversal, Command
Injection, Weak Hash, Unsafe Random) isn't hypothetical for this
codebase — this session's correctness pass already found and fixed one
real instance in the same family (the `mw_cors` CORS allow-list bug in
`ROADMAP_STATUS_UPDATED.md`, a silently-inert security control, not a
scanner-catchable pattern but the same "security feature that looks
present but doesn't actually work" category `kh audit` is meant to
catch), and separately confirmed `webi`'s `serve_static()` already
implements real path-traversal protection (rejects both literal `..`
and percent-encoded traversal attempts — see the phase3 test coverage
in `examples/webi_phase3_test.kh`). That's a genuine asset: a real,
working example of what "not vulnerable to path traversal" looks like
in this codebase, useful as a positive test fixture for the scanner
once it exists. What's still missing entirely: any static-analysis
infrastructure to build the scanner's actual detection logic on (see
§7 below — nothing like this exists yet), and (as Phase 3's doc already
found) any hashing primitives at all, so "Weak Hash" detection has
nothing to compare against — Kosh can't yet tell a caller "you used
MD5" vs "you used SHA-256" because *no* hash function is exposed to
Kosh scripts today, weak or otherwise.

### §5 / §9 / §14 (AI Compiler Assistant, AI Code Review, AI Documentation) — the LLM-calling half already exists; the code-understanding half doesn't

All three need two things: (a) a way to call an LLM, and (b) a way to
extract structured information about Kosh code to hand that LLM. Half
of this is real today — `packages/openai/openai.kh` is a genuine,
working API client (chat, embeddings, moderation, retry/backoff — not
a stub), so "hand a prompt to an LLM and get a response back" already
works, *given* an API key and outbound network access (the same
external-dependency caveat already noted for Phase 2/3's LLM-runtime
items — this is a hosted-API client, not a local model). What's
missing is (b): there's no call-graph extraction, no "explain this
function's role" structural analysis, nothing beyond the raw AST the
parser already builds internally and doesn't expose. `kh explain
parser.kh`'s "Call Graph" step specifically needs new tooling to walk
the AST and build that graph — a smaller, well-scoped piece of new work
sitting on an AST that already exists (the parser doesn't need to
change, just get introspected).

Also relevant: `main.c` has no interactive/REPL mode today — `kosh`
only runs a script file passed as `argv[1]all`. The "Warning → Apply?"
interaction style in §5 implies some kind of interactive prompt loop
(even if just at the CLI, not a full REPL) that doesn't exist in any
form yet.

### §7 Memory Analyzer / Static Analyzer — no existing pass to build on

Checked: the compiler pipeline today is parse → compile directly to
bytecode (`src/parser.c` → `src/compiler.c`), with no intermediate
analysis pass at all — no dead-code detection, no unused-variable
tracking, nothing that walks the AST looking for patterns before
code-gen. Both of these sections need that pass built from scratch.
Separately, the closures work from this session's correctness pass
(`ROADMAP_STATUS_UPDATED.md`) added Kosh's first ref-counted heap
structure (`KoshClosure`) — worth knowing about specifically *because*
a future memory analyzer would need to account for it as one of the
object kinds it can inspect, alongside the existing map/array/string
heap objects.

### §10–§12 (Build System, Cross-Compilation, Installers) — today's "build system" is a single makefile; both existing platform binaries were native-built, not cross-compiled

The whole build today is `make` against one `makefile`, invoking `gcc`
directly with a fixed source list (confirmed — no CMake, no build
generator, no per-target configuration). The Windows binaries already
in this repo (`kosh.exe`, `kh.exe`) reflect a genuinely different
`LDFLAGS` branch in the makefile (`winhttp` etc., visible in the
makefile's Windows section) — meaning they were almost certainly built
*on* Windows with a Windows toolchain, not cross-compiled from Linux.
`kh build --target windows` from a Linux machine needs an actual
cross-compilation toolchain wired in (e.g. `mingw-w64`), and `--target
android`/`arm64` need their own toolchains (NDK for Android)
entirely absent today. `.deb`/`.rpm`/`.apk`/`.pkg` packaging (§12) is
a separate, also entirely new layer on top of whatever `kh build`
produces — none of it exists yet, but the two-platform makefile split
that already exists is at least evidence the codebase is used to
maintaining platform-specific build logic in one place, which is a
reasonable foundation to extend rather than start over.

### §13 Language Server — the AST is a real foundation; the protocol layer is new

An LSP server needs exactly the data the parser already produces
(tokens, an AST, and — since this session's work — accurate line
numbers on runtime errors via the existing stack-trace mechanism, see
`ROADMAP_STATUS_UPDATED.md` §13) but exposed over the LSP JSON-RPC
protocol instead of used internally by the compiler. That protocol
implementation is net-new (no JSON-RPC/LSP code exists anywhere in
`src/` today), but it's the kind of "new transport layer over existing
internals" work `webi` itself is a working example of elsewhere in this
codebase — a plausible pattern to reuse, not a from-scratch design
problem.

### §17 Enterprise Security, §18 Plugin Marketplace — same gaps Phase 3 already found, restated here for completeness

- **Encrypted Bytecode / Code Signing:** needs the same crypto
  primitives (hashing, signing) Phase 3's doc found completely absent
  from `src/` — no change since that finding.
- **Capability-Based APIs:** a genuinely new idea relative to
  everything else in this document — it means Kosh code would need
  explicit permission grants to do sensitive things (file I/O, network
  calls, shell exec if §5 of Phase 3 ever lands) rather than the
  current model, where every native function is just globally callable
  the moment a package is imported (confirmed — `vm_global_set_native`
  registers natives with no permission/capability check anywhere in
  `src/vm_libs.c`). This is a real security-*model* change, not just a
  new feature, and would affect how every existing package works, not
  just new ones.
- **Plugin Marketplace:** same missing-`dlopen` gap as Phase 3 §10 —
  no dynamic loading mechanism exists anywhere in `src/` today.

### §6, §15 (Memory Analyzer's reference graph, VM Profiler) — the VM already tracks some of what these need internally, just not exposed

`src/vm.c`'s `CallFrame`/frame stack and the bytecode chunk's
instruction stream already carry a lot of what a profiler would report
(call depth, which function is executing, opcode being run) — they're
just not instrumented to *count* or *report* anything today, and
nothing captures timing. This is meaningfully less work than something
like the JIT or self-hosting: it's an instrumentation/reporting layer
on top of execution machinery that already exists and already runs
every opcode through one central `switch` statement (an easy place to
add counters), rather than a new execution model.

### §19 Complete AI Ecosystem, §20 Self-Optimizing Compiler, v3.0 vision diagram

§19 is a description of Phases 2 and 3 working together end-to-end,
not new scope on its own — its status is exactly the combined status of
those two documents. §20 is Phase 3 §3's JIT idea taken to its logical
conclusion (fully automatic, no manual "compile hot code" step) — same
prerequisite, same scale of effort, framed here as the payoff once §3
exists rather than a separate thing to build. The v3.0 diagram (AI
Core / Security / Networking sitting on the VM/runtime/platform layer)
is a fair summary of where Phases 1–3 already point this codebase, not
a new claim requiring its own reality-check.

### On stopping feature work after Phase 4

Directly consistent with, and a stronger version of, the same
suggestion already made at the end of the Phase 3 document — worth
repeating here because Phase 4 is where it would actually bind: by the
time self-hosting, an IDE/LSP, a security scanner, a build/cross-compile/
installer pipeline, and a plugin marketplace all exist on top of
Phases 1–3's tensor engine, autograd, distributed training, and JIT,
the surface area needing exactly the kind of systematic, run-everything-
and-check-the-real-output verification this session did for Phase 1
(see `ROADMAP_STATUS_UPDATED.md`) will be an order of magnitude larger
than it is today. The discipline of stopping to stabilize is easy to
state and, based on this project's own history so far (real, previously
invisible bugs found purely by finally testing thoroughly, twice now),
genuinely necessary rather than optional.
