# LLM (llama.cpp/GGUF bridge)

Real local LLM inference, via a native bridge to llama.cpp (libllama + ggml)
- not a mock, not a shell-out to a CLI. `src/llm_lib.c` links against
llama.cpp's public C API (`llama.h`) directly and runs real forward passes.

Kosh does not implement any tensor math, attention, or quantization here.
That is the point of bridging: GGUF files are already quantized upstream
(llama.cpp's k-quant schemes - Q4_K_M, Q5_K_M, Q8_0, etc. - calibrated,
per-block scaled) by whoever produced them, so accuracy-vs-size is solved
*before* Kosh ever sees the file. This is the fast path discussed against
the alternative (a native dtype/Tensor/autograd stack, which Kosh doesn't
have yet - see `docs/future plans/phase2-ai-foundation-plan.md`, section
17, which scopes quantization as its own later Phase 3 item).

For the Kosh-level API (`llm_load`/`llm_complete`/`llm_free`), see the
header comment in `src/llm_lib.h` - this doc covers getting it to build.

## This is a real build-time dependency - and a heavier one than `ocr`

Like OCR needing libtesseract, this needs llama.cpp built and present on
the machine you *build* Kosh on. Unlike libtesseract, there's no system
package / pkg-config path for it in most environments - llama.cpp is
normally vendored and built from source, not installed as a distro
package. So this is opt-in via `make LLM=1`, not part of the default
`make`/`make all` - the default build has zero new dependencies.

### Build steps (confirmed working, Linux x86_64)

```bash
git clone --depth 1 https://github.com/ggml-org/llama.cpp.git third_party/llama.cpp
cd third_party/llama.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_SERVER=OFF -DGGML_NATIVE=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build build --target llama -j$(nproc)
cd ../..
make LLM=1
```

`LLAMA_CPP_DIR` defaults to `third_party/llama.cpp`; override it
(`make LLM=1 LLAMA_CPP_DIR=/path/to/llama.cpp`) if you put the checkout
somewhere else. The `-fopenmp` in `LDFLAGS` matters: ggml's CPU backend is
built with OpenMP (`GOMP_barrier`/`GOMP_parallel` etc.), and the final
`kosh` link needs it too or those symbols stay undefined - this cost real
trial and error to find, so it's called out here rather than left implicit.

**Whether llama.cpp should be a git submodule, a plain vendored checkout
like the above, or something the build script fetches on demand is an
open decision, not made here** - same posture as this project's other
"reality check" docs leaving a structural choice explicit rather than
picking silently. A submodule pins an exact commit (llama.cpp moves fast
and occasionally breaks its own C API between versions) at the cost of
recursive-clone friction for anyone building Kosh.

### macOS / Windows

Not verified in this session (no macOS/Windows machine available to test
against). The cmake invocation above should be platform-agnostic, but the
`-fopenmp`/`-lstdc++` flags and static-lib paths in the makefile are
Linux-shaped and will likely need adjusting - flagging this as unverified
rather than guessing, same as the original Windows tesseract step was
flagged in `docs/ocr.md`.

### Heads up: `docs/ocr.md` currently says pkg-config wiring for tesseract
lives in "the makefile" - it doesn't, as of the current `makefile` (ocr
isn't in `SRCS` at all). Not something this change touches, but worth
knowing about since it's the same kind of doc/code drift this project
otherwise tracks carefully.

## What's verified vs. not (this session)

Real, confirmed by actually building and running it here:
- `llm_lib.c` compiles clean (`-Wall -Wextra`, zero warnings)
- Full `kosh` binary links successfully against real `libllama.a` +
  `libggml*.a`
- `llm_load()` on a bad path runs real llama.cpp code end-to-end (its
  actual `gguf_init_from_file`/`llama_model_load` error path fires,
  propagates back through `llm_lib.c`, and Kosh correctly receives `nil`)

Not verified - no GGUF model file was reachable from this sandbox (model
hosting is overwhelmingly on Hugging Face, which isn't on the network
allowlist here): the actual `llm_complete()` generation loop
(tokenize -> decode -> sample -> detokenize) has not been run against a
real loaded model. The code is written directly against the current
`llama.h` API (checked against the real header, not from memory), but
please test it against a real small GGUF file on your own machine before
relying on it.

## Known gaps (first cut - by design, not oversight)

- **Greedy sampling only.** No temperature/top-k/top-p/penalties yet -
  deterministic output. Real accuracy-preserving generation quality for
  anything beyond "does it produce sane text" will want at least temp +
  top-p; `llama_sampler_chain_add()` already supports chaining more
  samplers in, this just doesn't expose them to Kosh yet.
- **No multi-turn / session reuse.** Each `llm_complete()` call tokenizes
  and decodes the prompt from scratch; no KV-cache reuse across calls.
- **Fixed handle table** (`LLM_MAX_HANDLES = 4`), same style as ocr's
  `OCR_MAX_HANDLES` - bump it if you need more concurrent models.
- **No GPU offload wired up** (`n_gpu_layers` stays at the default from
  `llama_model_default_params()`) - this build only compiled ggml's CPU
  backend.
