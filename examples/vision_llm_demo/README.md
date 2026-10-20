# Vision + LLM + webi demo - one static binary

Real classical computer vision (`vision` package) + a real local LLM
(`llm_lib.c`, bridged to llama.cpp) + a real HTTP server (`webi` package) -
all compiled into Kosh's single `kosh` binary. No venv, no pip, no docker,
no system package manager involved at *run* time.

## Run it

```bash
# from the repo root, after: make LLM=1   (see docs/llm.md)
./kosh examples/vision_llm_demo/app.kh
# then: curl http://localhost:8123/analyze
```

`GET /` gives a one-line HTML page. `GET /analyze` runs real vision
analysis (dimensions, brightness histogram, blob count after
thresholding) on `sample.png`, and - if a real GGUF model is present -
asks it for a one-sentence description of what those stats suggest.

## Enabling the LLM part

Without a model file, `/analyze` still returns the full, real vision
analysis - it just says so instead of a description:

```json
"llm_description": "(no model loaded - place a real .gguf at model.gguf; see README.md)"
```

To enable it: put any real GGUF file at `examples/vision_llm_demo/model.gguf`
(any small instruct model works - this only sends it one short prompt).
Run `kosh` from the repo root (`./kosh examples/vision_llm_demo/app.kh`) -
`llm_load()` resolves its path relative to the process's working
directory, not the script's own directory the way vision's paths do; this
was found by actually running the demo end-to-end, not assumed.

This bridge was tested against a real model in the end: a small
hand-constructed synthetic GGUF (random weights, so gibberish output -
the point was proving the mechanism, not getting sensible text) loaded
and generated successfully through this exact code path. Test again with
a real trained model before relying on the output quality.

`model.gguf` in this folder *is* that exact test fixture (114 KB, 1-layer
tiny llama-arch, random f32 weights, byte-level vocab) - `/analyze` will
work out of the box against it, generating real (if repetitive/nonsense)
bytes, so you can confirm the whole pipeline on your own machine without
sourcing a real model first. `make_test_gguf.py` is the script that built
it (needs `pip install numpy gguf`) if you want to regenerate or tweak it
- swap it for a real instruct model's .gguf whenever you want actual
sensible output.

## The size/dependency comparison this demo exists for

The claim: getting vision + OCR/LLM + a web server working together in
Python usually means a venv, `pip install opencv-python
llama-cpp-python flask` (or similar), and whatever system libraries those
wheels need underneath. Kosh's version is one binary, built once, that
runs anywhere with no install step.

See the chat for the actual measured numbers (binary size, venv size,
install time) - this README documents the demo itself, not a snapshot of
numbers that will drift as package versions change.
