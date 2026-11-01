# Project structure

[← Back to the README](../README.md)

```
Kosh/
├── makefile                  # build (default; `make OCR=1` adds the Tesseract bridge)
├── configure, build.bat      # helper scripts
├── install.sh / install.ps1  # one-line installers (Linux/macOS, Windows)
├── kosh-installer.iss        # Inno Setup script for a Windows GUI installer
├── README.md  CHANGELOG.md  CONTRIBUTING.md  SECURITY.md  LICENSE
├── ROADMAP_STATUS_UPDATED.md # detailed, session-by-session engineering log
├── src/                      # the C11 implementation (see below)
├── packages/                 # 38 packages; registry.json lists them all
│   ├── registry.json
│   └── <name>/<name>.kh + package.json   (webi also has client/, secure*.kh, bot.kh)
├── docs/                     # reference docs, audits, plans (this directory)
├── tests/
│   ├── run_all.kh            # the core assertion suite
│   ├── suites/               # per-feature assertion suites
│   ├── test_*.kh             # package and native-library suites (crypto, aead, webi_*, ocr, vision, ...)
│   ├── regression_*.kh       # one file per fixed bug
│   ├── fixtures/             # generated test images
│   ├── fuzz/                 # parser fuzzers, memory stress, fuzz_webi.kh (Webi property fuzzer)
│   └── js/                   # Node tests for the browser client (vectors, e2e, jsdom, fuzz)
├── tools/
│   ├── gen_client_js.py      # embeds client/webi.js and proof.js into client_js.kh
│   └── gen_readme_packages.py # regenerates the package tables in README.md and docs/packages.md
├── examples/                 # example and demo scripts, OCR/vision fixtures, todo_app/
├── benchmarks/               # Kosh vs Python/Node/C, honestly reported (RESULTS.md)
├── playground/               # the browser playground (WebAssembly build)
├── assets/                   # logo and icons
├── Projects/                 # small apps written in Kosh (portfolio, task_tracker)
└── lib/                      # small shared and debug modules
```

### `src/`

```
lexer / parser / ast / token      # tokenizer, recursive-descent parser, AST
compiler / chunk                  # AST -> bytecode (38 opcodes)
vm / vm_libs / value              # the stack-based VM (the real runtime), value system, native registration
interpreter                       # legacy tree-walk interpreter (one import-parsing path only)
kosh_stdlib                       # 30+ built-in functions
json_lib, datetime_lib            # native JSON and datetime
requests_lib, net                 # native HTTP client
webi_lib                          # native HTTP server, MIME table, path-safe file resolution, rate limiter, DNS helpers
sqlite_lib                        # real libsqlite3 bridge (needs libsqlite3-dev)
vision_lib / vision_cv / vision_cascade / vision_font   # image I/O, filters, Haar-cascade detection
tensor_lib                        # Tensor type with autograd
crypto_lib, crypto_aead           # SHA-256/HMAC/PBKDF2/HKDF, ChaCha20-Poly1305, X25519
ocr_lib                           # Tesseract bridge (opt-in: make OCR=1)
llm_lib                           # llama.cpp bridge (opt-in: make LLM=1)
int8array_lib                     # int8 arrays for quantization
stb_image*.h                      # vendored stb libraries
main.c / main_vm.c / main_wasm.c  # entry points (native, VM-only, browser)
kh.c                              # the package manager CLI
```
