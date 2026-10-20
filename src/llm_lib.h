#ifndef KHAN_LLM_LIB_H
#define KHAN_LLM_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * llm_lib — local LLM inference backed by llama.cpp (libllama + ggml), the
 * same engine behind `llama-cli` / countless GGUF-based local-inference
 * tools.
 *
 * This is a genuine bridge, not a mock: it links against llama.cpp's public
 * C API (llama.h) and runs real forward passes through ggml. Khan does not
 * implement any tensor math, attention, or quantization itself here — the
 * whole point of bridging is that GGUF files are already quantized upstream
 * (llama.cpp's k-quant schemes: Q4_K_M, Q5_K_M, Q8_0, etc. — calibrated,
 * per-block scaled) by whoever produced them, so accuracy-vs-size is solved
 * *before* Khan ever sees the file. Khan's job is only to load the file and
 * run inference on it.
 *
 * Requires libllama.a / libggml*.a to be built (see docs/llm.md) and linked
 * into the `khan` binary. Hard build-time dependency, same posture as `ocr`
 * requiring libtesseract-dev: if the libs aren't there, the build fails
 * rather than silently degrading to a fake. Because ggml/llama.cpp are C++
 * internally, the *linker* for the final `khan` binary must be g++ (or gcc
 * with -lstdc++ added) even though this wrapper file itself is plain C —
 * see docs/llm.md for the exact makefile change.
 *
 * A "model" at the Khan level is a plain map:
 *   {"__llm_id": <handle>}
 * The underlying llama_model and llama_context pair lives in a native-side
 * table indexed by __llm_id. Always call llm_free() on models you no longer
 * need (each holds real RAM - the full de-quantized-on-the-fly compute
 * buffers plus the KV cache, not just the file's on-disk size).
 *
 * Registers:
 *   llm_load(path, [n_ctx])        -> model or nil
 *       n_ctx defaults to 2048 (context window in tokens). Loading fails
 *       (nil, message on stderr) if the file isn't a valid GGUF, or if
 *       there isn't enough RAM for the given n_ctx's KV cache.
 *   llm_complete(model, prompt, [max_tokens]) -> string or nil
 *       One-shot completion: tokenizes prompt, greedy-decodes up to
 *       max_tokens (default 256) or until the model's own end-of-generation
 *       token, returns the generated text (prompt not included). Greedy
 *       (argmax) sampling only in this first cut — deterministic, no
 *       temperature/top-k/top-p yet; see docs/llm.md's "known gaps" note.
 *       Each call is independent (no multi-turn KV-cache reuse yet).
 *   llm_free(model)                -> nil
 */
void llm_register_all(Environment *env);
void llm_register_all_vm(VM *vm);

#endif
