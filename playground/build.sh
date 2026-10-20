#!/bin/bash
# Rebuilds playground/khan.js + khan.wasm from src/.
#
# Requires emscripten (emcc). On Debian/Ubuntu:
#   apt-get install emscripten
# (if you hit a node-acorn/nodejs dependency conflict during that install -
#  common when a newer nodejs than Debian's packaged one is already
#  installed - `apt-get install -y --no-install-recommends emscripten`,
#  then `apt-get install -f` to pull in the real missing libs, works
#  around it; emscripten itself just needs *some* acorn npm package on
#  NODE_PATH to finish its post-link JS step, e.g. `npm install -g acorn`)
#
# This is a deliberately reduced build: no webi/requests (real socket
# I/O doesn't work in a browser sandbox without a lot of extra shimming)
# and no ocr (needs libtesseract compiled to WASM, which this doesn't
# attempt). Core language + stdlib + json/datetime/sqlite(mock)/vision.

set -e
cd "$(dirname "$0")/.."

emcc -std=c11 -Wall -Wextra -O2 -Isrc -D_POSIX_C_SOURCE=200809L \
  -Wno-implicit-function-declaration -Wno-builtin-declaration-mismatch -Wno-cast-function-type \
  src/lexer.c src/parser.c src/ast.c src/chunk.c src/value.c src/compiler.c src/vm.c src/vm_libs.c \
  src/khan_stdlib.c src/json_lib.c src/datetime_lib.c src/sqlite_lib.c \
  src/vision_lib.c src/vision_cv.c src/vision_cascade.c \
  src/main_wasm.c \
  -o playground/khan.js \
  -s EXPORTED_FUNCTIONS='["_run_khan_source","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME='createKhanModule' \
  -s ENVIRONMENT='web'

echo "Built playground/khan.js + playground/khan.wasm"
