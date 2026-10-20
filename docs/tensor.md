# Tensor + autograd (v1)

The first slice of Kosh's AI-native-core direction (see `docs/future
plans/phase2-ai-foundation-plan.md`): a real native tensor type with
automatic differentiation, not a library bolted onto a general-purpose
language.

```
import "tensor"

let x = Tensor([[1, 2], [3, 4]], true)    # requires_grad = true
let y = Tensor([[5, 6], [7, 8]], true)

let loss = x.add(y).mul(x).sum()
loss.backward(nil)

print loss.data()       # 100
print x.grad.data()     # [[7, 10], [13, 16]]
print y.grad.data()     # [[1, 2], [3, 4]]
```

## Why this, and why now

`phase2-ai-foundation-plan.md` names the Tensor Engine and Automatic
Differentiation as the two 5/5-starred, foundational pieces of the whole
AI-native direction — "without Tensor, no AI." Its own Part 2
(reality-check) lists what was missing to build it properly: `x.grad`,
`y.backward()`, and `tensor.to(device)` all need method-call syntax on an
object, and Kosh had no classes or method calls at the time that doc was
written. Both now exist (see `docs/classes.md`) — classes were built in
the session immediately before this one — so this is the first thing
built once that blocker was actually clear, not a restart.

## What works

| Feature | Notes |
|---|---|
| `VAL_TENSOR` | A real value type: one contiguous `double*` buffer + shape, not nested Kosh arrays. See `src/tensor_lib.c`/`.h`. |
| `Tensor(data, requires_grad)` | `data` is a number (0-D) or a rectangular nested array (1-D, 2-D, or deeper for construction/elementwise use). |
| `.add()` / `.sub()` / `.mul()` | Elementwise. Either side can be a plain number or a single-element tensor, broadcast against the other — see "Broadcasting" below for the exact rule. |
| `.matmul()` | 2-D only (see "What does not"). |
| `.transpose()` | 2-D only. |
| `.reshape(shape)` | Any shape with the same total element count. |
| `.sum()` | Reduces to a 0-D (scalar) tensor. |
| `.relu()` / `.sigmoid()` / `.tanh()` | Elementwise activations, full backward support. |
| `.softmax()` | Row-wise, numerically stable (subtracts the row max before `exp`). **Inference only** — does not build a graph node; training a classifier goes through `cross_entropy_loss` in `docs/nn.md` instead, which fuses softmax and cross-entropy for a much simpler gradient. |
| `_tensor_argmax` / `_tensor_sqrt` / `_tensor_div` | Native-level only (not `Tensor` methods) — `argmax` returns a plain Kosh array of per-row indices, not a tensor; `sqrt`/`div` exist to support `nn.kh`'s `Adam` optimizer. |
| `.data()` / `.shape()` / `.ndim()` / `.numel()` | Inspection; `.data()` converts back to a plain Kosh number/array. |
| `.backward(grad)` | `nil` seeds with ones (the usual `loss.backward(nil)` entry point for a scalar loss); an explicit `Tensor` reseeds a mid-graph node. Accumulates into `.grad`, so a tensor used more than once in a graph correctly sums the contributions from every use (`y = x.add(x)` gives `dy/dx = 2`, not `1`). |
| `==` on two tensors | Real value equality (same shape and contents), unlike every other container type in Kosh (arrays/maps/classes/instances compare by identity) — see `values_equal`'s `VAL_TENSOR` case in `value.c`. |
| `type(x)` | `"tensor"`. |
| Native error handling | Every shape/argument error prints a message and returns `nil` rather than crashing — the same convention `vision_lib.c`/`sqlite_lib.c` already use for their natives, which is why these aren't yet catchable with `try`/`catch` (see "What does not"). |

### Broadcasting

Three rules, checked in order:

1. **Same shape** → ordinary elementwise.
2. **A plain number against a tensor of any shape** → broadcast the
   number against every element, result keeps the tensor's shape exactly
   (including when the tensor has exactly one element — see "A real bug"
   below for why that last part needed its own fix).
3. **Two 2-D tensors whose dimensions are each either equal or 1** → the
   usual row/column broadcast (`(1,n)` against `(m,n)`, `(m,1)` against
   `(m,n)`, `(1,1)` against anything 2-D). This is what makes a shared
   `Dense` layer bias work against a batch.

Anything else — mismatched sizes with no dimension that's 1, more than
2-D with unequal shapes, etc. — is a shape-mismatch error. There is no
general N-D numpy-style broadcasting beyond the 2-D case above.

Backward for a broadcast operand reduces the incoming gradient back down
to that operand's own shape by **summing over whichever axis was
broadcast** (`_tensor_sum_to_shape` in `tensor_lib.c`), not a flat
sum-to-scalar — see "A second real bug" below for why that distinction
matters.

## What does not (yet)

- **Broadcasting is exactly the three rules above, no further.** Beyond
  2-D, or with a mismatched dimension that isn't 1, it's a shape-mismatch
  error — no general N-D numpy-style broadcasting.
- **`matmul`/`transpose` are 2-D only.** No batched matmul, no N-D
  transpose with an axis permutation.
- **No GPU, no dtype system.** Everything is `double`.
- **Tensor shape/argument errors aren't catchable.** They print to
  stderr and the native function returns `nil`; nothing throws, so
  `try`/`catch` can't intercept them the way it can a Kosh-level
  `throw`. Silent `nil` propagation is possible if a caller doesn't
  check for it — same tradeoff the project already accepted for
  `vision_lib.c`/`sqlite_lib.c`.
- **`nn`-style layers now ship separately** — see `docs/nn.md` for
  `Dense`, `ReLU`/`Sigmoid`/`Tanh`, `Sequential`, `mse_loss`, and `SGD`
  (`packages/nn/nn.kh`), built entirely on top of this package.

## How it works

**Value.** `VAL_TENSOR`'s `Obj` carries `data` (owned `double*`), `shape`
(owned `int*`, length `ndim`), `ndim`, and `size` (element count; a 0-D
tensor has `ndim == 0`, `shape == NULL`, `size == 1`). Reference-counted
like arrays/maps — `value_copy` bumps the refcount, `value_free` frees
both buffers at zero. `print`/`str()` render it as nested brackets
matching its shape; `values_equal` is real value equality (the one
exception among Kosh's container types, documented above and in
`value.c`), since a tensor is conceptually a number, not a shared,
mutable container.

**Native engine** (`src/tensor_lib.c`, registered via
`tensor_register_all_vm`, called from `main.c` alongside the other
`*_register_all_vm` libraries). Functions are named `_tensor_*` — a
leading underscore, the same convention `webi_lib.c` uses for internals
the Kosh-level API wraps — because user code is meant to go through the
`Tensor` class, not call these directly. `_tensor_from_nested`/
`_tensor_to_nested` convert between a plain/nested Kosh array and the
flat buffer via a generic recursive depth-detector
(`nested_depth`/`nested_fill_shape`/`nested_validate_shape`), not a
hardcoded 0/1/2-D special case, so `reshape` can produce (and
`to_nested` convert back) tensors of any dimensionality even though
*construction* from a literal is most commonly 0/1/2-D in practice.
Every function follows the established error convention (see the table
above) rather than inventing a new one.

**Kosh-level `Tensor` class** (`packages/tensor/tensor.kh`, replacing the
old pure-Kosh nested-array version entirely — nothing else in the repo
imported it, confirmed with a repo-wide search before replacing it).
Each op (`add`/`sub`/`mul`/`matmul`/`transpose`/`reshape`/`sum`) builds
the result tensor, then records `.op` (a string tag) and `.a`/`.b` (the
input `Tensor`s) on it — a tiny computation graph, built from ordinary
instance fields rather than closures. `backward(grad)` dispatches on
`.op` with one `if`/`elif` per operation, computing and recursing into
each input's gradient with the standard reverse-mode rule for that op
(e.g. for `z = a @ b`: `dL/da = g @ bᵀ`, `dL/db = aᵀ @ g`). Every
gradient in `tests/suites/tensor.kh` is checked against a hand-worked
value, not just "runs without crashing."

**Backward-compatible free functions.** `matmul`/`dot`/`add`/`scale`
(nested-array in, nested-array out) are kept at the bottom of
`tensor.kh` for anything written against the old package's API — they
now call through the native engine instead of the old package's pure-Kosh
`O(n^3)` loops, so they're also just faster.

A real bug, found by valgrind while writing the stress test for this: the
first version of the training-loop stress test called `loss.data()[0][0]`
on a **0-D** tensor's `.data()`, which returns a plain number, not a
nested array — indexing it threw an uncaught fatal runtime error on the
very first iteration, which looked like a 20+-record valgrind leak
because the VM's fatal-exit path doesn't unwind the stack. That leak
report was a symptom of the test's bug, not the tensor engine's; fixing
the test (summing `.data()` directly) and rerunning showed 0 leaks across
2000 real forward/backward iterations. Worth remembering: an unexplained
cluster of valgrind leaks right after a "Runtime error" line almost
always means a fatal exit skipped cleanup, not a real per-iteration leak
— check for the error first.

A second real bug, this one in the test *suite*, not a script: naming a
test helper `add` inside `tests/suites/stdlib_collections.kh` silently
overwrote the global `add` that `packages/tensor/tensor.kh` defines,
because nested `fn` declarations in Kosh still define a *global* (see
`docs/classes.md`), and every suite file gets fully imported before any
suite function is *called* — so the last-called suite that happens to
redefine a popular name like `add` wins, however unrelated its own file
is. Fixed by renaming the test's internal helper to `_reduce_add_cb`;
worth remembering for any future suite that picks a common name for a
local helper.

A third real bug, found while building `packages/nn/nn.kh` on top of
this (not while writing a test for *this* package — one layer up, where
the feature actually got used end to end): the original broadcasting
rule only handled "one side has exactly one element." The very first
attempt at training a model — a single-neuron `Dense(1,1)`, the simplest
possible case — revealed something worse: once an optimizer step did
`w.buf = _tensor_sub(w.buf, ...)` on that size-1 weight, `tensor_binop`
treated the *weight itself* as an interchangeable bare scalar and
collapsed its result to a 0-D tensor, permanently discarding the
weight's real `(1,1)` shape. Fixed by making a tensor's own shape always
win over a plain number's lack of shape, and making two differently-
shaped size-1 tensors keep whichever has the more specific (higher-ndim)
shape, rather than ever producing a bare 0-D result when a real tensor
was involved on either side.

Fixing that then exposed a second, bigger gap in the same area: any
`Dense` layer with `out_features > 1` failed outright, because its bias
(shape `(1,n)`) couldn't broadcast against a batch output (shape
`(m,n)`) at all — only same-shape or single-scalar broadcasting existed.
Added the row/column 2-D broadcast rule above to fix the forward pass,
which immediately exposed a *third*: the bias's gradient, flowing back
through `add`'s backward with no shape adjustment, inherited the batch's
`(m,n)` shape instead of the bias's own `(1,n)`, which then corrupted the
bias itself the next optimizer step — the same class of bug as the
weight-collapse above, but for a shape that actually matters in practice
(any real batch-trained layer), not just a size-1 edge case. Fixed with
`_tensor_sum_to_shape`, which reduces a gradient back to an operand's
shape by summing over the specific axis that was broadcast, not a flat
sum-to-scalar. All three are exercised directly in
`tests/suites/tensor.kh`'s `suite_tensor_broadcasting`, including a
hand-computed bias-gradient check (`dL/db` for `loss = sum(x@w + b)`
across a 2-row batch), and end-to-end in `tests/suites/nn.kh` by actually
training a multi-output-neuron layer on a batch and checking its
parameters' shapes stay correct afterward.

The throughline across all three sessions of bugs in this feature (the
`OP_CALL_SUPER` borrowed-reference free in `docs/classes.md`, the
`OP_PRINT` leak, and these three): **the real bugs showed up by actually
using the feature for its stated purpose** — training something, however
small — not by writing unit tests against the feature's own stated
scope. A unit test only catches a gap between the code and its own
documented contract; it was *training a single-neuron model* and *then a
multi-neuron one* that found the three bugs here, because that's what
actually exercises an optimizer mutating a live parameter's buffer,
which no amount of testing `_tensor_add` in isolation would surface.

## Tests

`tests/suites/tensor.kh`, registered in `tests/run_all.kh`:

- **`suite_tensor_native`** (26 checks) — construction from nested
  arrays/scalars, shape/ndim/numel, every elementwise op and scalar
  broadcast, matmul, transpose, sum, reshape (both directions), value
  equality, ones/zeros-like, and every native error path.
- **`suite_tensor_autograd`** (21 checks) — forward and backward for
  add/mul (including the reused-tensor double-accumulation regression),
  matmul backward, transpose/reshape backward, mixing a `Tensor` with a
  plain number, a small `Linear`-style layer, a 2,000-iteration
  forward/backward stress loop checked for exact numeric correctness
  (and separately confirmed leak-free under valgrind), and the
  backward-compatible free functions.
- **`suite_tensor_broadcasting`** (9 checks) — regression coverage for
  all three bugs above: size-1 shape preservation through sub/mul, a
  number broadcasting against a size-1 tensor, row and column broadcast
  against a batch, and the hand-computed bias-gradient reduction check.

`tests/suites/nn.kh` (`suite_nn`, 13 checks) covers `packages/nn/nn.kh`
directly — see `docs/nn.md`.

Every expected gradient in the suite was verified by hand first (shown
in the commit/session notes), not just asserted to match whatever the
code happened to produce.
