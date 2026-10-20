# nn — a minimal neural network module (v1)

Built entirely on top of `packages/tensor/tensor.kh` (see `docs/tensor.md`)
— no new VM primitives beyond the activation natives `tensor.kh` already
wraps. Matches the "Neural Network Module" / "Loss Functions" items in
`docs/future plans/phase2-ai-foundation-plan.md`, scoped down to what's
actually implemented and tested here.

```
import "nn"

let model = Sequential([Dense(2, 8), ReLU(), Dense(8, 1)])
let opt = SGD(model.parameters(), 0.1)

let pred = model.forward(x)
let loss = mse_loss(pred, target)
opt.zero_grad()
loss.backward(nil)
opt.step()
```

## What works

| Piece | Notes |
|---|---|
| `Dense(in_features, out_features)` | A linear layer: `x.matmul(w).add(bias)`. Weights are shape `(in_features, out_features)`, uniform-random in `[-0.1, 0.1]` (`random()`, not a true Gaussian — see "What does not"). Bias is shape `(1, out_features)`, zero-initialized, and broadcasts against a batch via the row-broadcast rule in `docs/tensor.md`. |
| `ReLU()` / `Sigmoid()` / `Tanh()` | Activation layers — thin wrappers around `Tensor.relu()`/`.sigmoid()`/`.tanh()` so they slot into a `Sequential` the same way `Dense` does. No parameters. |
| `Sequential(layers)` | Chains `.forward()` calls; `.parameters()` aggregates every layer's parameters (skipping layers with none) for handing straight to an optimizer. |
| `mse_loss(pred, target)` | `mean((pred - target)^2)`. |
| `cross_entropy_loss(logits, target)` | `target` is a plain Kosh array of class indices (`[0, 2, 1, ...]`), not one-hot — matches how most frameworks' own `CrossEntropyLoss` take it. Internally fuses softmax + cross-entropy into one graph node with the standard closed-form gradient `(probs - one_hot(target)) / N`, the same reason real frameworks fuse these two (numerical stability, and a far simpler gradient than composing softmax's full Jacobian with a separate log). |
| `accuracy(logits, target)` | Fraction of rows where `argmax(logits)` matches `target`. Not part of the autograd graph — for reporting, not training. |
| `SGD(params, lr)` | `.step()` applies `param.buf -= lr * param.grad.buf` directly on each parameter's buffer (deliberately *not* through a `Tensor` op — an optimizer step must not itself be recorded in the autograd graph). `.zero_grad()` clears every parameter's `.grad`. |
| `Adam(params, lr)` | Standard Adam: per-parameter moving averages of the gradient and its square (`beta1=0.9`, `beta2=0.999`, `eps=1e-8`), with bias correction. Same `.step()`/`.zero_grad()` interface as `SGD`. |

Verified end to end, not just "runs without crashing":

- A `Dense(1, 1)` model trained for 300 steps on `y = 2x + 1` drops its
  loss by over 100x.
- A `Sequential([Dense(2, 8), ReLU(), Dense(8, 1)])` trained for 3,000
  steps solves XOR (loss `< 0.01`) — XOR is **not linearly separable**,
  so this is a genuine test that the hidden layer, `ReLU`'s backward, and
  the row-broadcast bias fix in `tensor_lib.c` are all correct together,
  not just that the code executes.
- A `Sequential([Dense(3, 16), ReLU(), Dense(16, 3)])` trained with
  `Adam` + `cross_entropy_loss` for 300 steps on a 10-example, 3-class
  synthetic task reaches 100% accuracy (loss drops from ~1.10 to ~3e-8).
  Unlike the two tests above, this one came back clean on the first try
  — the three broadcasting bugs in `docs/tensor.md` were already fixed
  by the time `Adam`/`cross_entropy_loss` were built on top of them.

## What does not (yet)

- **No `Conv2D`/`MaxPool`/`AvgPool`/`Embedding`/`BatchNorm`/`LayerNorm`/
  `Dropout`/`LSTM`/`GRU`/`Transformer`.** Only `Dense` and the three
  elementwise activations.
- **No `BinaryCrossEntropy`, `MAE`, `Huber`, `KLDiv`.** Only `mse_loss`
  and `cross_entropy_loss`.
- **`cross_entropy_loss` is single-label only** — one correct class per
  row (a class-index array), not multi-label (no per-class independent
  probabilities / `BCEWithLogitsLoss`-style target).
- **No `AdamW`, `RMSProp`, `Adagrad`.** Only `SGD` and `Adam`.
- **No dataset/DataLoader API.** You build your input/target `Tensor`s
  yourself.
- **Weight init is uniform, not Gaussian**, and there's no Xavier/He
  scaling by layer size — fine for the small examples here, not
  something to rely on for a deep network.
- Inherits every `tensor.kh` limitation (2-D-only `matmul`, errors not
  catchable with `try`/`catch`, etc.) — see `docs/tensor.md`.

## How it works

Each layer class implements the same informal interface:
`forward(x)` and `parameters()`. `Dense` returns `[self.w, self.bias]`;
the activation layers return `[]`. `Sequential` doesn't need to know
which is which — it just concatenates whatever every layer's
`parameters()` gives back, which is what lets `model.parameters()` hand
an optimizer exactly the right list regardless of how the layers are
mixed.

`SGD.step()` mutates `param.buf` directly via `_tensor_sub`/`_tensor_mul`
rather than calling `param.sub(...)` — using the `Tensor` op would
record the update itself as a node in the autograd graph, which is not
what an optimizer step should do (the parameter's *new* value isn't a
function of anything that should be differentiated through later).

Three real bugs came out of building this — not from testing `nn.kh`
itself, but from actually training the two simplest possible models
(`Dense(1,1)` for linear regression, then anything with
`out_features > 1` trained on a batch) and watching both fail. All three
turned out to live in `tensor_lib.c`'s broadcasting, not in this
package — see "A third real bug" in `docs/tensor.md` for the full story.
This package is what drove finding them: the bugs were invisible to
`tensor.kh`'s own test suite because that suite never built an optimizer
that mutates a live parameter's buffer in a loop.

## How `cross_entropy_loss` fits into the autograd graph

Unlike `mse_loss` (which is just `.sub().mul().mean()` — ordinary
composed `Tensor` ops), `cross_entropy_loss` builds its output node
directly: it calls the native `_tensor_softmax`/`_tensor_cross_entropy_loss`
itself, then sets the result `Tensor`'s `.op`, `.a`, and two extra fields
(`._probs`, `._target`) that only this one op uses, by reaching into
`Tensor`'s fields directly — Kosh has no real encapsulation, so a
function outside `tensor.kh` can do this as long as `tensor.kh`'s own
`backward()` has a matching `"softmax_cross_entropy"` case to interpret
those fields, which it does (see `packages/tensor/tensor.kh`). This
keeps the fused gradient's numerical-stability and simplicity advantage
(`(probs - one_hot(target)) / N`, no backprop through a separate
softmax Jacobian) without needing a more general "custom op" mechanism
in `tensor.kh` itself.

## Tests

`tests/suites/nn.kh`, registered in `tests/run_all.kh`:

- **`suite_nn`** (24 checks) — `Dense`/`ReLU` shapes and parameter
  counts, `Sequential` aggregating parameters across mixed layer types,
  `mse_loss` forward and backward, `SGD.step()`/`.zero_grad()` behavior,
  the `Dense(1,1)` linear-regression convergence check, the XOR
  convergence check, `softmax`/`argmax`, `cross_entropy_loss` forward and
  backward (checked against the same hand-computed values as
  `tensor_lib.c`'s own native-level numbers), `accuracy`, `Adam`'s
  `.step()`/`.zero_grad()`, and the end-to-end multi-class classifier
  regression test described above.
