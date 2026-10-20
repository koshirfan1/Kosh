# Khan Playground

A browser-based way to try Khan with nothing installed — the real
lexer/parser/compiler/VM, compiled to WebAssembly with Emscripten, running
client-side. No server executes any code; everything happens in the
visitor's own tab.

## Deploying it

Three static files, no build step at request time: `index.html`,
`khan.js`, `khan.wasm`. Any static host works — GitHub Pages, Netlify,
Vercel, a plain `python3 -m http.server`, whatever's convenient. Nothing
here needs a backend.

**MIME type matters**: the host must serve `khan.wasm` as
`application/wasm` (GitHub Pages, Netlify, etc. already do this
correctly for `.wasm` by default). If your instantiation is slow or
falls back to the non-streaming path, that's the first thing to check.

## Rebuilding from source

`./build.sh` (from this directory, or anywhere — it `cd`s to the repo
root itself). Requires `emcc`; see the comment at the top of `build.sh`
for a Debian/Ubuntu install note if `apt-get install emscripten` fights
you over a `node-acorn`/nodejs version conflict, which it very
plausibly will if a newer nodejs than Debian's own packaged one is
already on the machine.

## What's deliberately not in this build

This links a **reduced** set of Khan's native libraries — not the same
36-package, everything-included `khan` binary the CLI produces:

- **No `webi` / `requests`.** Both need real socket I/O, which doesn't
  exist in a browser's WASM sandbox without substantial extra shimming
  (proxying through `fetch`/WebSockets, effectively rewriting the
  networking layer). Out of scope for a first playground.
- **No `ocr`.** Needs libtesseract, which isn't compiled to WASM here —
  a real undertaking on its own (tesseract.js exists as a *separate*,
  already-WASM-ported project; bridging Khan's own C code to a
  WASM-compiled libtesseract would be its own project, not a build flag).

**What does work**: the core language (functions, closures, control
flow, arrays/maps — anything not needing a file or a socket), the
standard library, `json`, `datetime`, the `sqlite` package (it's a mock
that never touched real SQL anyway, so nothing is lost here), and
`vision`'s pixel-math functions (blank-canvas operations work; loading
an actual image *file* doesn't, since there's no real filesystem —
Emscripten's virtual filesystem could change that later, but wasn't
part of this pass).

## A real bug this surfaced

Building this exposed a genuine latent bug in `src/compiler.c`: it
called `khanfn_register()`/`khanfn_registry_index()` (declared in
`vm.h`) without including `vm.h` at all, so the compiler fell back to
an implicit `int`-returning declaration that didn't match the real
`void`-returning function. Harmless on native x86 — the calling
convention tolerates it by accident — but WASM enforces function
signatures strictly, so this was a hard crash (`RuntimeError:
unreachable`) the moment a compiled script called either function.
Fixed by adding the missing `#include "vm.h"` to `compiler.c` — this is
a real fix to the shared compiler, not something specific to the WASM
build, and it's already included in the native build too (verified: all
existing test suites still pass after the fix).
