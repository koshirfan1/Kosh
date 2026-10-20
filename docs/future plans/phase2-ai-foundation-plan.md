# Phase 2 plan — AI Foundation (v1.5)

Status: **planning input, not yet scoped into an implementation plan.**
This document records the Phase 2 proposal as given by the project
owner's brother, translated and organized, followed by a reality-check
of what it actually requires from the current codebase — the same way
`docs/phase4-plan.md` and `docs/phase5-hardening-and-design-plan.md`
verify facts against the source before proposing an approach. Nothing in
Part 1 has been built yet; Part 2 is what has to be true before any of
it can be.

**Phase goal, as given:** turn Kosh into an AI/ML development language.

---

## Part 1 — The proposal, as given

### 1. Tensor Engine ⭐⭐⭐⭐⭐

The foundational piece — "without Tensor, no AI."

```
a = tensor([[1,2],[3,4]])
b = tensor([[5,6],[7,8]])

print(a+b)
print(a@b)
```

Operations: `add`, `subtract`, `multiply`, `divide`, `matmul`,
`transpose`, `reshape`, `flatten`, `concatenate`, `broadcast`.

Internal layering, as given:

```
Tensor
  ↓
Shape
  ↓
Stride
  ↓
Data Pointer
  ↓
dtype
```

### 2. Data Types

Support: `int8`, `int16`, `int32`, `int64`, `float16`, `float32`,
`float64`, `bool`. Future: `bfloat16` (useful for AI workloads).

### 3. GPU Backend

CPU first, then CUDA / OpenCL / Vulkan Compute.

```
device = gpu()
tensor.to(device)
```

### 4. Automatic Differentiation ⭐⭐⭐⭐⭐

The backbone of deep learning.

```
x = tensor(5, requires_grad=true)
y = x*x
y.backward()
print(x.grad)   # 10
```

The autograd engine is meant to be the foundation for everything after it.

### 5. Neural Network Module

```
model = Sequential([
    Dense(784,256),
    ReLU(),
    Dense(256,10)
])
```

Layers: `Dense`, `Conv2D`, `MaxPool`, `AvgPool`, `Embedding`,
`BatchNorm`, `LayerNorm`, `Dropout`, `LSTM`, `GRU`, `Transformer`.

### 6. Loss Functions

`MSE`, `MAE`, `CrossEntropy`, `BinaryCrossEntropy`, `Huber`, `KLDiv`.

### 7. Optimizers

`SGD`, `Momentum`, `Adam`, `AdamW`, `RMSProp`, `Adagrad`.

### 8. Dataset API

```
dataset = ImageFolder("./cats")
loader = DataLoader(dataset, batch=32, shuffle=true)
```

Source formats: CSV, JSON, Images, Audio, Video, Text.

### 9. Model Saving

```
model.save("cat.km")
model.load("cat.km")
```

Custom format: `.km` ("Kosh Model").

### 10. Tokenizer

For LLM use. Support: BPE, SentencePiece, WordPiece.

### 11. LLM Runtime ⭐⭐⭐⭐⭐

Called out as especially important given the repository's direction.

```
model = llm.load("llama.gguf")
response = model.chat("Hello")
```

Supported formats: GGUF, ONNX, Safetensors (future).

### 12. Computer Vision

The repo already has a `vision` package — expand it. Target functions:
Resize, Crop, Rotate, Blur, Sharpen, Canny, Contours, OCR, Face
Detection, Object Detection.

### 13. Audio

Formats: WAV, MP3, FLAC. Features: FFT, Spectrogram, Mel Spectrogram, MFCC.

### 14. AI Utilities

Built-in: Random Seed, Dataset Split, Metrics (Accuracy, Precision,
Recall, F1, Confusion Matrix).

### 15. Benchmark Suite

Standard models to benchmark against automatically: MNIST, CIFAR10,
IMDB, TinyShakespeare.

### 16. Documentation

A dedicated docs set: `docs/ai.md`, `docs/tensor.md`, `docs/autograd.md`,
`docs/llm.md`, `docs/vision.md`.

### 17. Future — Phase 3

Training, Distributed AI, Multi-GPU, Model Parallelism, Inference
Server, Quantization, LoRA, Fine-tuning.

### The core architectural advice, as given

> Write AI algorithms in Kosh itself, but don't write heavy numerical
> kernels in Kosh.

Specifically **not** in Kosh: Matrix Multiplication, Convolution, FFT,
SIMD, GPU kernels. These belong in native C/C++; Kosh should only expose
a high-level API over them.

```
x = Tensor.rand(4096,4096)
y = Tensor.rand(4096,4096)
z = x @ y
```

but internally:

```
↓
Native C
↓
SIMD
↓
CUDA/OpenCL
↓
CPU/GPU
```

The stated goal: Python-like developer experience, C-level performance —
the same direction modern AI runtimes take.

---

## Part 2 — Reality check against the current codebase

Facts below were checked directly against `src/` and `packages/` as they
stand today (after the correctness-fix session recorded in
`ROADMAP_STATUS_UPDATED.md`), not assumed. The short version: **almost
none of the syntax in Part 1's examples runs today, and the gap isn't
just "no tensor package yet" — it's language-level.**

### There is already a `tensor` package, but it doesn't match this vision

`packages/tensor/tensor.kh` exists today and exports `matmul`, `dot`,
`add`, `scale` — but it's a thin, pure-Kosh convenience layer:

- Tensors are plain nested Kosh arrays (`[[1,2],[3,4]]`), not a real
  `Tensor` type with shape/stride/dtype/data-pointer as Part 1 describes.
- `matmul` is a naive triple-nested `while` loop (`O(n³)`, no
  vectorization, no native backend) — see the function body in
  `packages/tensor/tensor.kh`.
- No `subtract`, `divide`, `transpose`, `reshape`, `flatten`,
  `concatenate`, or `broadcast` — only the four ops listed above exist.
- No dtype concept at all — every number in Kosh is a single native
  `double` (see `Value`'s `VAL_NUMBER` in `src/interpreter.h`); there is
  no `int8`/`float16`/etc. distinction anywhere in the value system.

This is a real starting point (naming, package structure, and the
`matmul`/`dot`/`add` surface can likely be kept and extended), but it is
**not** close to load-bearing for autograd, GPU dispatch, or the dtype
system Part 1 describes.

### The language itself is missing several prerequisites Part 1's examples assume

Checked directly against `src/lexer.c`, `src/parser.c`, `src/ast.h`:

- **No `@` operator.** `a@b` / `x @ y` (matmul syntax) doesn't lex —
  `@` isn't a token anywhere in `src/lexer.c`. Would need a new operator
  added to the lexer, parser, and compiler (same three layers touched
  when fixing the `**`-operator gap noted in
  `ROADMAP_STATUS_UPDATED.md`), or the examples need to become ordinary
  function calls (`matmul(x, y)`) instead.
- **No classes/objects.** There is no `class` keyword, no `AST_CLASS`
  node, nothing resembling struct/object definitions anywhere in the
  parser. `Dense(784,256)`, `Sequential([...])`, and `model.save(...)`
  all read as "construct an object, call a method on it" — none of
  that exists. Today's closest equivalent is a plain map plus functions
  that take it as their first argument (the pattern already used
  throughout `webi` — e.g. `res_json(...)`, `route(app, ...)`), which
  works but reads nothing like the examples in Part 1.
- **No method-call syntax.** `y.backward()`, `x.grad`, `tensor.to(device)`
  — dot-call syntax (`value.method(...)`) doesn't exist; `.` is only used
  for map-literal-style field access in a few native response builders,
  never for calling a function "on" a value. Every existing package
  (including `tensor`, `math`, `strings`) uses free functions
  (`str_repeat(s, n)`, not `s.repeat(n)`).
- **No default or keyword arguments.** `tensor(5, requires_grad=true)`
  relies on a keyword argument with a default; the parser was directly
  tested this session and rejects keyword-argument call syntax entirely
  (`Expected ')' after arguments` on `inner(c: 100)`). Every Kosh
  function today takes a fixed positional arity, strictly checked at
  call time (`Arg count mismatch` if it doesn't match exactly — this
  strictness is itself relied on elsewhere, e.g. it's what caught two
  stale test files this session).
- **No operator overloading.** `a+b`/`a@b` on a `tensor(...)` value
  would need `OP_ADD`/a new matmul opcode in `src/vm.c` to special-case
  a tensor value type, the same way `OP_ADD` today special-cases
  `VAL_STRING` vs `VAL_NUMBER` — there's no generic
  "dispatch based on runtime type" mechanism for user-defined types,
  because there's no user-defined-type mechanism at all yet.

None of this means Part 1's syntax is a bad target — it's a reasonable,
Python-like surface. It means **the tensor/autograd/NN work described in
sections 1–9 cannot start from "add a tensor package"** the way, say,
adding `uuid` or `logger` did. It needs either (a) real language
features added first (operators, some notion of typed/object values,
method-call or at least a consistent free-function convention that
covers grad tracking cleanly), or (b) the whole surface redesigned
around what Kosh can already express (free functions + maps), accepting
that it won't look exactly like the Part 1 examples.

### The "native kernels, Kosh-level API" architecture is sound and already the house style — with one caveat

Part 1's closing advice (heavy numeric kernels in native code, Kosh only
exposes the API) matches how every other capability in this codebase
already works — `vision`, `sqlite`, `requests`, `datetime`, JSON, and
the rest are all thin `.kh` wrappers over C implementations
(`src/vision_lib.c`, `src/sqlite_lib.c`, etc.) registered as native
functions in the VM (`vm_global_set_native`, see `src/vm_libs.c`). This
part of the plan requires no new architecture — it's the proven,
existing pattern.

The caveat: **there is no FFI or dynamic-loading mechanism.** Every
native capability today is a `.c` file statically compiled directly
into the `kosh` binary via the `makefile`'s source list — confirmed,
there's no `dlopen`/plugin/foreign-function-interface code anywhere in
`src/`. That's fine for CPU kernels (SIMD-optimized matmul, FFT, etc.
can be added as another statically-linked `.c` file exactly like
`vision_lib.c` was), but CUDA/OpenCL/Vulkan Compute backends bring real
build-system questions this codebase hasn't had to answer yet: optional
compilation (a machine without a GPU/CUDA toolkit still needs to build
`kosh` at all — see the CPU-first sequencing already called out in
Part 1, §3), and per-platform linking (`nvcc`/CUDA toolkit availability
varies far more than the POSIX/Windows split the existing `#ifdef`
pattern in `webi_lib.c`/`main.c` handles today).

### Computer vision (§12) has more of a running start than anything else in Part 1

`packages/vision` is real and substantial today — confirmed 41 exported
functions across `packages/vision/*.kh`, including most of what §12
asks for by name or close equivalent: `resize_file`, `crop_file`,
`rotate_file`, `blur_file`/`blur_gaussian_file`, `sharpen_file`,
`edges_file` (Canny-family edge detection), `find_blobs`/
`find_blobs_after_threshold`/`largest_blob` (contour-like blob analysis),
`detect_faces`/`detect_faces_tuned`/`detect_with_cascade`
(Haar-cascade face detection), plus thresholding, morphology,
histograms, and template location that weren't asked for but are
already there. **Missing from §12's list**: OCR, and general (non-face)
object detection — everything else is at minimum a naming/wrapper task,
not new capability. Note the whole package operates on file *paths*
(`resize_file(path, ...)`), not an in-memory tensor/image object — worth
deciding early whether §1's `Tensor` type should become the common
currency vision ops read/write, since that's a design fork the other
mostly-unbuilt sections don't yet force a decision on.

### Everything else in Part 1 (§2–§11, §13–§17) has no existing code to build from

Checked and confirmed absent: no dtype system, no GPU/`device` concept,
no autograd, no `Sequential`/layer/loss/optimizer anything, no
`DataLoader`/dataset API, no `.km` model format, no tokenizer, no LLM
runtime (GGUF/ONNX loading), no audio package, no benchmark suite. These
are all genuinely greenfield, in roughly the dependency order Part 1
already puts them in (tensor → autograd → NN modules → training loop
pieces is the right order; LLM runtime and audio are independent side
branches that don't block on the NN stack).

### Suggested next step

Given the language-level gaps above are the actual blocker for
sections 1, 4, and 5 (the three ⭐⭐⭐⭐⭐/core sections), the highest-value
next planning document is probably a **language extensions plan**
scoped narrowly to what sections 1/4/5 need — likely: one new binary
operator (`@`), a minimal typed-value mechanism (doesn't need full
classes; could be as small as a new `VAL_TENSOR` value type handled the
same way `VAL_STRING`/`VAL_MAP` already are in `src/value.c`/`src/vm.c`),
and a decision on whether method-call syntax is worth adding or whether
the whole NN API leans into Kosh's existing free-function convention
instead (`backward(y)` instead of `y.backward()` — smaller, but a
real departure from Part 1's examples, worth the project owner and
brother agreeing on explicitly before code gets written either way).
