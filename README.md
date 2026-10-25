# Kosh Programming Language

<p align="center"><img src="assets/kosh-icon-256.png" width="128" height="128" alt="Kosh logo"></p>

![Language](https://img.shields.io/badge/language-C11-blue)
 ![Build](https://img.shields.io/badge/build-make-green)
![License](https://img.shields.io/badge/license-MIT-orange)
![Status](https://img.shields.io/badge/status-pre--1.0%20active%20development-yellow)
![Packages](https://img.shields.io/badge/packages-37-purple)

**Kosh** is a custom, indentation-based programming language built entirely from scratch in C11 — lexer, parser, bytecode compiler, and VM, with no dependency on any other language's runtime. Created by **Irfan Khan**, this project is a hands-on exploration of compiler design, virtual machine implementation, and standard library engineering, with a 37-package ecosystem spanning web development, classical computer vision, real OCR, and network protocols. The language draws inspiration from Python's clean indentation syntax and Lua's simplicity, while keeping a minimal footprint in pure C.

Honest framing, since this README makes a point of not overstating things: Kosh is **pre-1.0** by its own [roadmap](#roadmap)'s criteria. It's a real, working, multi-session-hardened language and runtime, not a toy — but if you're picking a language to ship something in tomorrow, this isn't that. If you want to see how a real bytecode VM and a real package ecosystem get built from nothing, read on.

> **Repository**: [github.com/koshirfan1/Kosh](https://github.com/koshirfan1/Kosh)

> **Naming note:** the language is called **Kosh** (project name `kosh-lang`). The interpreter command is `kosh`, the package manager is `kh`, and source files keep the `.kh` extension. The GitHub repository lives at [`koshirfan1/Kosh`](https://github.com/koshirfan1/Kosh), and packages install to `~/.kosh`.

---

## Table of Contents

- [Overview](#overview)
- [What's Real, What's a Known Gap](#whats-real-whats-a-known-gap)
- [Quick Start](#quick-start)
- [Package Manager — kh](#package-manager--kh)
- [Packages](#packages)
- [Web Framework — webi](#web-framework--webi)
- [Language Features](#language-features)
- [Syntax & Examples](#syntax--examples)
- [Built-in Libraries](#built-in-libraries)
- [Standard Library](#standard-library)
- [Project Structure](#project-structure)
- [Building from Source](#building-from-source)
- [Architecture](#architecture)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

Kosh is a from-scratch programming language implementation focused on understanding the fundamentals of how programming languages work under the hood. The project implements a complete language pipeline:

1. **Lexer (Tokenizer)** — Converts raw source code into a stream of tokens
2. **Parser** — Builds an Abstract Syntax Tree (AST) using recursive descent parsing
3. **Compiler** — Compiles the AST into bytecode (38 opcodes)
4. **Bytecode VM** — A stack-based virtual machine executes the compiled bytecode — this is the real, primary execution engine (an earlier tree-walk interpreter still exists in the source tree, but only for one legacy import-parsing path; it is not what runs your program)
5. **Standard Library** — 30+ built-in functions for I/O, strings, math, arrays, and maps
6. **Native Libraries** — C libraries bridging to real external engines: JSON, datetime, HTTP, SQLite-shaped storage, classical computer vision (with real Haar-cascade face detection), and OCR via Tesseract
7. **Package Manager (`kh`)** — Install community packages with `kh install <name>`

## What's Real, What's a Known Gap

Most from-scratch language projects are tempted to round up. This one tries hard not to — a prior version of the `vision` package shipped a `detect_faces()` that returned two hardcoded boxes regardless of input, and the fix was to rip it out and build real Haar-cascade detection, not to keep it and call it done. That history is worth being upfront about, both the mistake and the fix.

**Real, verified, not mocked:**
- The bytecode compiler + VM — the actual execution path for every script you run
- `vision`'s face detection — real Viola-Jones cascade detection, not hardcoded output
- `ocr` — a genuine native bridge to libtesseract; feeds it real pixel data, not a wrapper around the `tesseract` CLI
- `sqlite` — a real bridge to `libsqlite3` (`sqlite3_prepare_v2`/`sqlite3_bind_*`/`sqlite3_step`), not a JSON-backed mock (an earlier version was; ripped out and rebuilt for real)
- `{}` maps — a real open-addressing hash index (FNV-1a), not a linear scan; the old O(n) version made a 50,000-key map take ~9 seconds, the current one takes ~0.01s (~725x). Full story in [docs/hash-table-audit.md](docs/hash-table-audit.md)
- `try`/`catch`/`throw` — a real exception-handling implementation (unwinds the VM stack and call frames to the nearest handler), not just a parser accepting the syntax. Catches both user `throw`s and every built-in runtime error. Design and a real bug found along the way in [docs/trycatch-implementation.md](docs/trycatch-implementation.md)
- `class` — Python-style classes: `fn` methods with an explicit `self`, `__init__` constructors, dynamic fields, method chaining, single inheritance (`class Dog(Animal):`) with `super`, all working with `try`/`catch`. No multiple inheritance or bound-method values yet. See [docs/classes.md](docs/classes.md)
- `Tensor` — a real native tensor type (`VAL_TENSOR`: one contiguous buffer, not nested arrays) with automatic differentiation: `add`/`sub`/`mul`/`matmul`/`transpose`/`reshape`/`sum`/`relu`/`sigmoid`/`tanh`, `.backward()`, `.grad`, row/column broadcasting (e.g. a shared bias across a batch). The first slice of Kosh's AI-native-core direction. 2-D-only matmul, no GPU. See [docs/tensor.md](docs/tensor.md)
- A cycle collector for circular references in arrays/maps (`a[0] = a` no longer leaks for the life of the process) — a Bacon-Rajan trial-deletion collector, the same algorithm CPython's `gc` module uses, not a full tracing GC. Two real bugs found and fixed during testing, plus a >99% leak reduction measured (not just "doesn't crash") in [docs/gc-notes.md](docs/gc-notes.md)
- Testing: thousands of fuzz-mutated parser inputs with zero crashes, memory stress-tested at 10M-allocation scale with flat RSS, CI gating on real assertion suites across Linux/Windows/macOS

**Known, open gaps — not hidden, not fixed yet:**
- String concatenation in a loop is **partially** fixed — a redundant
  length-rescan on the growing accumulator was removed (a real ~2.4x
  win, verified), but the deeper cause is still open:
  `OP_SET_LOCAL`/`OP_SET_GLOBAL`/`OP_SET_UPVALUE` deep-copy the
  assigned value on every write by design, so building a string with
  repeated `+=` in a loop is still worse than linear overall. Full
  story in [benchmarks/RESULTS.md](benchmarks/RESULTS.md)'s
  `string_concat` section.
- Closures are value-capture only — a nested function can read an
  enclosing variable, but there's no shared mutable upvalue, so a
  "counter closure" pattern that mutates captured state doesn't work
  the way it does in Python/JS.

If a claim in this README turns out to not match reality, that's a bug in the README — [open an issue](https://github.com/koshirfan1/Kosh/issues).

**On performance, specifically**: Kosh is generally slower than Python and Node right now, not faster — roughly 2-8x slower on tight loops, competitive only on JSON round-trips (where it's actually ~2.4x faster than Python). Computed-goto dispatch was tried as the obvious next lever and **measured slower, not faster**, than the existing switch-based dispatch (5-15% depending on benchmark) — reverted; full measurement and why in [docs/dispatch-perf.md](docs/dispatch-perf.md). The next real lead is a `value_copy()` call on every basic operation (even a plain number) — not yet profiled, flagged as the most promising thing to actually measure next. Full numbers and methodology in [benchmarks/RESULTS.md](benchmarks/RESULTS.md).

---

## Quick Start

**Want to try it before installing anything?** [Try Kosh in your browser](https://koshirfan1.github.io/Kosh/) — the real compiler and VM, compiled to WebAssembly with Emscripten, running client-side with no server involved. (Deployed via GitHub Pages from `playground/` — see [.github/workflows/deploy-playground.yml](.github/workflows/deploy-playground.yml). If that link 404s, GitHub Pages needs a one-time manual enable that a workflow file can't do on its own — confirmed by an actual failed deploy, not just a guess: repo Settings → Pages → Source → "GitHub Actions", then re-run the workflow from the Actions tab.) See [playground/README.md](playground/README.md) for what's and isn't included in that build.

### Install

**Linux / macOS:**
```bash
curl -fsSL https://raw.githubusercontent.com/koshirfan1/Kosh/main/install.sh | bash
```

**Windows (PowerShell):**
```powershell
irm https://raw.githubusercontent.com/koshirfan1/Kosh/main/install.ps1 | iex
```
Needs a GCC toolchain on `PATH` already (MSYS2/MinGW-w64) — the script checks for one and tells you exactly what to install if it's missing, rather than silently installing a toolchain for you. **Caveat, stated plainly**: this script was written and reviewed carefully but has not been run on an actual Windows machine in the environment it was authored in (no such access there) — same class of caveat this project's CI workflow already carries for its own Windows steps. If something in it doesn't work as described, that's a bug — please open an issue with what happened.

**Windows (GUI installer, alternative to the PowerShell script above):** [kosh-installer.iss](kosh-installer.iss) is an [Inno Setup](https://jrsoftware.org/isinfo.php) script that produces a proper double-click `kosh-setup.exe` — a setup wizard with an **"Add Kosh to PATH" checkbox** (checked by default), a Start Menu entry, and a normal Add/Remove Programs uninstaller, closer to Python's own official Windows installer experience than a terminal one-liner is. It packages already-built `kosh.exe`/`kh.exe` rather than compiling them itself (see the comment at the top of that file for the exact build-then-compile steps). [.github/workflows/release.yml](.github/workflows/release.yml) automates exactly that build-then-compile sequence and can publish the result to Releases (see just below) — until a tagged release actually exists there, building `kosh-setup.exe` yourself via the free Inno Setup Compiler is the only way to get one. Same not-yet-verified-on-real-Windows caveat as `install.ps1` above applies here too.

There's no *published* pre-built binary release yet (see the [roadmap](#roadmap) — v1.0 hasn't shipped) — [.github/workflows/release.yml](.github/workflows/release.yml) can build one (`kosh.exe`, `kh.exe`, and a compiled `kosh-setup.exe` from the Inno Setup script above) and publish it to this repo's [Releases page](https://github.com/koshirfan1/Kosh/releases) automatically whenever a version tag (`v0.010.0`-style) is pushed, but that hasn't been done yet either. Once it has, downloading a release asset from there is the fastest path on Windows — no compiler needed. Until then, both scripted installers below build from source rather than downloading a binary, the way `rustup`/`deno`'s installers do when there's nothing prebuilt for your platform yet. Neither one runs `sudo`/an elevated prompt on your behalf — they check for what they need and tell you the exact command for your platform if something's missing. Read [install.sh](install.sh) / [install.ps1](install.ps1) before piping either into a shell, same as you should for any curl-pipe installer — that's not Kosh-specific advice.

After it finishes — restart your shell on Linux/macOS (or `source` the rc file it mentions); on Windows, `kosh` is already on `PATH` in the same PowerShell window, new windows get it automatically too:

```bash
kosh --version
kosh                    # starts an interactive REPL — see "Interactive REPL" below
kosh path/to/script.kh

# Install packages
kh install math
kh install strings
kh install colors
kh install requests
kh install postman
```

```kosh
import "math"
import "colors"

print green("Kosh is ready!")
print math_sqrt(144)
```

**Prefer to build it yourself, or the installer doesn't fit your setup?**

```bash
git clone https://github.com/koshirfan1/Kosh.git
cd Kosh
make                    # builds kosh(.exe) and kh(.exe) in the current directory
make install            # Linux/macOS only — installs both to ~/.kosh/bin, same as install.sh's last step.
                        # On Windows, use install.ps1 instead (`make install` prints why and stops).
```

Without `make install`, run the binaries directly as `./kosh script.kh` (Linux/macOS) or `kosh.exe script.kh` (Windows) from inside the checkout.

---

## Package Manager — kh

Kosh ships with `kh.exe` — a package manager that downloads packages from the official registry hosted on GitHub.

### Commands

| Command | Description |
|---|---|
| `kh install <name>` | Download and install a package |
| `kh remove <name>` | Remove an installed package |
| `kh list` | Show all available packages in the registry |
| `kh installed` | Show locally installed packages |
| `kh info <name>` | Show details about a package |

### How it works

- Packages are installed to `C:\Users\<you>\.kosh\packages\<name>\` on Windows, or `~/.kosh/packages/<name>/` on Linux/macOS
- The registry lives at `packages/registry.json` in this repo
- `import "<name>"` in any `.kh` file automatically resolves installed packages

```bash
kh install math
```
```kosh
import "math"
print math_sqrt(144)    # 12
print PI                # 3.14159...
```

---

## Packages

37 packages, installed the same way regardless of category: `kh install <name>`, then `import "<name>"`.

### Core & CLI

| Package | Version | Description |
|---|---|---|
| `math` | 2.0.1 | Advanced math: sqrt, pow, gcd, primes, factorial, mean |
| `strings` | 1.0.0 | String utilities: split, trim, replace, pad, repeat, contains, join |
| `collections` | 1.0.0 | Functional array helpers: sort, filter, map_each, reduce, find, unique, chunk, zip, sort_by |
| `colors` | 1.0.0 | Terminal ANSI colors: red, green, bold, boxes, etc. |
| `argparse` | 1.0.0 | Command-line argument parser |
| `validation` | 1.0.0 | Data validation: emails, lengths, numeric checks |
| `dotenv` | 1.0.0 | Load environment variables from `.env` files |
| `uuid` | 1.0.1 | Unique ID generation: uuid_v4, short IDs, named entity IDs, sequential counters |
| `events` | 1.0.1 | Event emitter: on, emit, once, off, history |
| `logger` | 1.0.1 | Structured logger: debug/info/warn/error levels, file output, silent mode |
| `test` | 1.0.0 | Unit testing framework: assertions, test suites, pass/fail reporting |
| `datetime` | 1.0.1 | Date/time utilities: timer, elapsed, human-readable duration, sleep helpers |

### Web — the `webi` ecosystem

| Package | Version | Description |
|---|---|---|
| `webi` | 1.2.0 | Web framework: routing, security, templates, static files, HTTP server — see [docs/webi.md](docs/webi.md) |
| `webi_auth` | 1.0.0 | Authentication middleware for webi |
| `webi_socket` | 1.0.0 | WebSocket support for webi |
| `morgos` | 1.0.0 | Morgan-style request logger for webi — colored status, elapsed time, response size, as a swappable `after()` hook |

### HTTP, APIs & testing

| Package | Version | Description |
|---|---|---|
| `requests` | 1.1.0 | HTTP client with auto JSON decode: get, post, post_json, put, delete |
| `postman` | 1.0.0 | API testing toolkit: send requests, assert responses, run test suites |
| `swagger` | 1.0.0 | Swagger UI and API documentation generator |
| `openai` | 2.0.0 | Production-grade OpenAI API client: chat completions, multi-turn conversations, embeddings, moderation, model listing, structured errors, retry/backoff |

### Data & storage

| Package | Version | Description |
|---|---|---|
| `fs` | 1.0.1 | File system utilities: read, write, append, copy, path helpers, JSON config |
| `csv_io` | 1.0.0 | CSV file reader and writer |
| `json_db` | 1.0.0 | Simple NoSQL database using JSON files |
| `orm` | 1.0.0 | Simple Object-Relational Mapper for JSON databases |
| `sqlite` | 1.0.0 | SQL database bridge — real `libsqlite3` bridge, requires `libsqlite3-dev` to build |

### Computer vision & OCR

| Package | Version | Description |
|---|---|---|
| `vision` | 3.0.0 | Full classical CV toolkit: image I/O, filters (blur/sharpen/emboss/edges), thresholding (fixed/Otsu/adaptive), morphology, blob detection, histograms, drawing, template matching, HSV color-range detection, real Haar-cascade face detection |
| `ocr` | 1.1.0 | Real OCR via Tesseract: text, word-level bounding boxes, orientation auto-correction, searchable PDF export, character whitelisting, multi-language — see [docs/ocr.md](docs/ocr.md) |

### AI / ML utilities

| Package | Version | Description |
|---|---|---|
| `tensor` | 1.0.0 | Real native tensors with autograd (`VAL_TENSOR`, not nested arrays) — see [docs/tensor.md](docs/tensor.md) |
| `nn` | 1.0.0 | Minimal neural-network module (Dense, activations, Sequential, CrossEntropy, SGD, Adam) on top of `tensor` — see [docs/nn.md](docs/nn.md) |
| `kbrain` | 1.0.0 | Simple machine learning and AI utilities |
| `nlp` | 1.0.0 | Natural language processing and sentiment analysis |
| `quant` | 1.0.0 | Accuracy-preserving quantization: per-channel calibration, error measurement, bias correction, outlier-aware sparse decomposition, mixed-precision strategies |

### Networking & protocols

| Package | Version | Description |
|---|---|---|
| `dns` | 1.0.0 | DNS lookup utilities using DNS-over-HTTPS |
| `ftp` | 1.0.0 | FTP client for file transfers |
| `grpc` | 1.0.0 | gRPC client for high-performance RPC |
| `mqtt` | 1.0.0 | MQTT protocol for IoT messaging |
| `smtp` | 1.0.0 | Email sending utilities |
| `ssh_client` | 1.0.0 | SSH client for remote command execution |

Full detail on the packages below is in their own docs where noted; everything else is documented inline in its `.kh` source under `packages/<name>/`.

### math

```kosh
import "math"

print math_sqrt(144)        # 12
print math_pow(2, 10)       # 1024
print math_factorial(6)     # 720
print math_gcd(48, 18)      # 6
print math_is_prime(97)     # true
print math_mean([1,2,3,4])  # 2.5
print PI                    # 3.14159265...
print E                     # 2.71828182...
```

### strings

```kosh
import "strings"

print str_trim("  hello  ")               # hello
print str_replace("foo bar foo", "foo", "baz")  # baz bar baz
print str_split("a,b,c", ",")            # ["a", "b", "c"]
print str_join(["x", "y", "z"], " | ")   # x | y | z
print str_starts_with("hello", "hel")    # true
print str_reverse("Kosh")                # hsoK
print str_pad_left("5", 3, "0")          # 005
```

### colors

```kosh
import "colors"

print red("error occurred")
print green("build passed")
print yellow("warning: low memory")
print bold("important message")
print_success("Tests passed")
print_error("File not found")
print_warn("Deprecated API")
print_info("Server started on port 8080")
```

### requests

```kosh
import "requests"

let res = get("https://api.example.com/users")
if ok(res):
    let users = json(res)
    print users[0]["name"]
else:
    raise_for_status(res)

# POST JSON
let data = {"name": "Irfan", "lang": "Kosh"}
let res2 = post_json("https://api.example.com/users", data)
print status(res2)
```

### postman

```kosh
import "postman"

let pm = pm_new_with_base("https://jsonplaceholder.typicode.com")

pm = pm_get(pm,      "GET todo",   "/todos/1")
pm = pm_post_json(pm, "POST todo", "/todos", {"title": "Learn Kosh", "completed": false})
pm = pm_assert(pm,   "math works",  1 + 1 == 2)

let fake = {"status": 200, "body": "{\"id\": 1}", "success": true}
pm = pm_expect_status(pm,     "status 200",  fake, 200)
pm = pm_expect_json_field(pm, "id equals 1", fake, "id", 1)

pm_run(pm)
```

Output:
```
  Kosh Postman ─ Test Results
  ══════════════════════════════════════

  PASS  GET todo  [200]
  PASS  POST todo  [201]
  PASS  math works  [0]
  PASS  status 200  [200]
  PASS  id equals 1  [200]

  ══════════════════════════════════════
  Total: 5   Passed: 5   Failed: 0
  ══════════════════════════════════════
  ✓ All 5 tests passed!
```

---

## Web Framework — webi

`webi` is a Flask-inspired web framework: routing, request/response
helpers, middleware, opt-in security (CSRF, rate limiting, API keys,
CORS), HTML templates (including reading them off disk), static file
serving, and a small HTTP server — all built on top of Kosh.

```bash
kh install webi
```

```kosh
import "webi"

let app = webi_app()
app = route(app, "GET", "/", fn_index)
app = serve_static(app, "/static", "public")

fn fn_index(req):
    let name = query_get(req, "name", "World")
    return res_html(render("<h1>Hello, {{name}}!</h1>", {"name": name}))

webi_run(app, 8080)
```

Or pull in the security/HTTP-client/JSON helpers in one line with
[`from`-import](docs/from-import.md):

```kosh
from webi import webi, security, requests, json
```

**Full reference:** [docs/webi.md](docs/webi.md) — routing, request/response
helpers, middleware, CSRF/rate-limiting/API-key/CORS security,
`render()`/`render_file()`, `serve_static()`, running the server, and
known limitations (single-threaded server, string/NUL-byte caveats for
binary static assets).

---

## Language Features

### Core Language

- ✅ **Indentation-based scoping** — Spaces define block structure (like Python)
- ✅ **Variable declarations** — `let` keyword
- ✅ **Function definitions** — `fn` keyword with parameters and closures
- ✅ **Recursion** — Functions can call themselves
- ✅ **Conditionals** — `if` / `elif` / `else`
- ✅ **While loops** — `while condition:`
- ✅ **For-in loops** — `for var in expr:`
- ✅ **Break & Continue** — `break` and `continue` inside loops
- ✅ **Return statements** — `return expr` from functions
- ✅ **Closures** — Functions capture their enclosing lexical scope
- ✅ **Import/Module system** — `import "filename.kh"` or `import "packagename"`, plus [`from "pkg" import A, B, C`](docs/from-import.md) for pulling out individual symbols, the package's own name, or a sibling submodule file
- ✅ **String escape sequences** — `\n`, `\t`, `\r`, `\\`, `\"`, `\0`, `\xHH`

### Data Types

- ✅ **Numbers** — Integers and floating-point (double precision)
- ✅ **Strings** — Double-quoted with full escape sequence support
- ✅ **Booleans** — `true` / `false`
- ✅ **Nil** — `nil`
- ✅ **Arrays** — `[1, 2, 3]` with zero-based indexing
- ✅ **Maps** — `{"key": value}` dictionaries

### Operators

- ✅ **Arithmetic** — `+`, `-`, `*`, `/`, `%`
- ✅ **Comparison** — `==`, `!=`, `<`, `<=`, `>`, `>=`
- ✅ **Logical** — `and`, `or`, `not`
- ✅ **String concatenation** — `+`
- ✅ **Indexing** — `arr[i]`, `map["key"]`

---

## Syntax & Examples

### Variables & Types

```kosh
let name    = "Irfan"
let age     = 25
let active  = true
let nothing = nil
let scores  = [95, 87, 92]
let person  = {"name": "Irfan", "city": "Faisalabad"}
```

### Conditionals with elif

```kosh
fn grade(score):
    if score >= 90:
        return "A"
    elif score >= 80:
        return "B"
    elif score >= 70:
        return "C"
    else:
        return "F"

print grade(85)   # B
```

### Loops with break and continue

```kosh
# break
let i = 0
while true:
    if i >= 5:
        break
    i = i + 1
print i   # 5

# continue — skip evens
for n in [1, 2, 3, 4, 5, 6]:
    if n % 2 == 0:
        continue
    print n   # 1 3 5
```

### Recursion

```kosh
fn factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

print factorial(10)   # 3628800

fn fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

print fib(10)   # 55
```

### String Escape Sequences

```kosh
print "Hello\nWorld"         # newline
print "Tab:\there"           # tab
print "Quote: \"Hi\""        # double quote
print "\x1b[31mRed\x1b[0m"  # ANSI color (hex escape)
```

### JSON (built-in)

```kosh
let data = {"name": "Kosh", "version": 2, "ready": true}
let s = json_encode(data)
print s   # {"name": "Kosh", "version": 2, "ready": true}

let decoded = json_decode(s)
print decoded["name"]   # Kosh
```

### Datetime (built-in)

```kosh
let today = now()
print date_format(today, "%Y-%m-%d")       # 2026-06-28
print date_format(today, "%A, %B %d %Y")   # Saturday, June 28 2026

let next_week = date_add(today, "days", 7)
print date_format(next_week, "%Y-%m-%d")
```

### HTTP Requests (built-in)

```kosh
let res = http_get("https://api.example.com/data")
print res["status"]    # 200
print res["success"]   # true
print res["body"]      # raw response body

let post_res = http_post_json("https://api.example.com/users",
                               {"name": "Irfan", "lang": "Kosh"})
print post_res["status"]
```

---

## Built-in Libraries

These are native C libraries included in `kosh.exe` — no installation needed.

### JSON

| Function | Description |
|---|---|
| `json_encode(value)` | Any Kosh value → JSON string |
| `json_decode(string)` | JSON string → Kosh map/array/value |

### Datetime

| Function | Description |
|---|---|
| `now()` | Current local time as a map (`year`, `month`, `day`, `hour`, `minute`, `second`, `weekday`, `timestamp`) |
| `utcnow()` | Same but UTC |
| `timestamp()` | Unix timestamp as a number |
| `date_format(date, fmt)` | Format using strftime codes (`%Y-%m-%d`, `%H:%M:%S`, `%A`, etc.) |
| `date_parse(str, fmt)` | Parse a date string into a date map |
| `date_add(date, unit, n)` | Add `"seconds"`, `"minutes"`, `"hours"`, `"days"`, or `"weeks"` |
| `date_diff(d1, d2)` | Difference in seconds |

### HTTP Requests

| Function | Description |
|---|---|
| `http_get(url)` | GET request |
| `http_post(url, body)` | POST with form-encoded body |
| `http_post_json(url, map)` | POST with auto-encoded JSON body |
| `http_put(url, body)` | PUT request |
| `http_delete(url)` | DELETE request |

All HTTP functions return `{"status": 200, "body": "...", "success": true}`.

> **Windows**: Uses WinHTTP (built-in, no extra install).
> **Linux/macOS**: Uses `curl` (must be installed).

---

## Standard Library

30+ native built-in functions available in every Kosh program:

### Type Conversion

| Function | Description |
|---|---|
| `num(x)` | String → number |
| `str(x)` | Any value → string |
| `type(x)` | Returns type name: `"number"`, `"string"`, `"bool"`, `"nil"`, `"array"`, `"map"` |

### String Functions

| Function | Description |
|---|---|
| `len(s)` | Length of string, array, or map |
| `substring(s, start, length)` | Extract substring (note: 3rd arg is **length**, not end index) |
| `upper(s)` | Uppercase |
| `lower(s)` | Lowercase |
| `contains(s, sub)` | Check if string contains substring |
| `trim(s)` | Strip leading/trailing whitespace |
| `split(s, delim)` | Split string into array |

### Array Functions

| Function | Description |
|---|---|
| `push(arr, val)` | Return new array with value appended |
| `range(n)` | Array `[0, 1, ..., n-1]` |
| `range(start, end)` | Array `[start, ..., end-1]` |

### Map Functions

| Function | Description |
|---|---|
| `keys(map)` | Array of all keys |
| `has(map, key)` | Check if key exists |

### Math Functions

| Function | Description |
|---|---|
| `abs(x)` | Absolute value |
| `min(a, b)` | Minimum |
| `max(a, b)` | Maximum |
| `sqrt(x)` | Square root |
| `round(x)` | Round to nearest integer |
| `floor(x)` | Round down |
| `ceil(x)` | Round up |
| `pow(base, exp)` | Exponentiation |
| `random()` | Random float in [0, 1) |

### I/O Functions

| Function | Description |
|---|---|
| `input()` | Read a line from stdin |
| `input(prompt)` | Show prompt then read line |
| `read_file(path)` | Read file as string |
| `write_file(path, content)` | Write string to file |
| `file_exists(path)` | Check if file exists |

### Utility Functions

| Function | Description |
|---|---|
| `sleep(ms)` | Pause execution (milliseconds) |
| `clock()` | CPU time in seconds |
| `exit(code)` | Exit program |

---

## Project Structure

```
Kosh/
├── kosh.exe                  # Kosh runtime (Windows) — compiler + VM
├── kh.exe                    # Kosh package manager (Windows)
├── makefile                  # Build configuration
├── README.md                 # This file
├── ROADMAP_STATUS_UPDATED.md # Detailed, session-by-session engineering log
├── docs/
│   ├── webi.md                            # Full webi framework reference
│   ├── ocr.md                             # ocr package: setup, platform notes, API
│   ├── from-import.md                     # `from X import A, B, C` reference
│   ├── opcodes.md                         # The 38 bytecode opcodes
│   ├── onnx-ocr-plan.md                   # Plan for a future ONNX inference bridge
│   ├── phase4-plan.md                     # Plan: threaded webi server
│   ├── phase5-hardening-and-design-plan.md # Plan: webi security hardening
│   ├── memory-notes.md                    # Memory-management audit notes
│   ├── hash-table-audit.md                # Findings on the {} map type (see known gaps)
│   ├── parser-robustness-audit.md         # Fuzz-testing findings
│   └── call-overhead-audit.md             # Function-call performance audit
├── benchmarks/
│   └── RESULTS.md             # Kosh vs Python/Node/C, honestly reported
├── packages/                  # all 37 live here; registry.json lists them all
│   ├── registry.json
│   ├── math/  strings/  collections/  colors/  argparse/  validation/
│   ├── dotenv/  uuid/  events/  logger/  test/  datetime/
│   ├── webi/  webi_auth/  webi_socket/  morgos/
│   ├── requests/  postman/  swagger/  openai/
│   ├── fs/  csv_io/  json_db/  orm/  sqlite/
│   ├── vision/  ocr/
│   ├── tensor/  kbrain/  nlp/
│   └── dns/  ftp/  grpc/  mqtt/  smtp/  ssh_client/
├── examples/                  # 40+ example/test scripts, plus:
│   ├── hello.kh
│   ├── vision_demo.kh          # face detection, filters, thresholding walkthrough
│   ├── vision_test_all.kh      # visual (manual-inspection) vision test
│   ├── ocr_test.png / ocr_paragraph.png / ocr_french.png  # OCR test fixtures
│   └── todo_app/               # a small real app written in Kosh
├── tests/
│   ├── run_all.kh              # the core assertion suite
│   ├── test_vision.kh          # automated vision suite
│   ├── test_ocr.kh             # automated ocr suite
│   └── suites/                 # per-feature assertion suites
└── src/
    ├── lexer.h / lexer.c              # Lexer — tokenizer
    ├── ast.h / ast.c                  # AST node definitions
    ├── parser.h / parser.c            # Recursive descent parser
    ├── compiler.h / compiler.c        # AST → bytecode compiler
    ├── chunk.h / chunk.c              # Bytecode chunk (constants + instructions)
    ├── vm.h / vm.c / vm_libs.c        # The stack-based bytecode VM — the real runtime
    ├── interpreter.h / interpreter.c  # Legacy tree-walk interpreter (one import-parsing path only — not what runs your program)
    ├── kosh_stdlib.c                  # 30+ built-in functions
    ├── json_lib.h / json_lib.c        # Native JSON encode/decode
    ├── datetime_lib.h / datetime_lib.c    # Native datetime functions
    ├── requests_lib.h / requests_lib.c    # Native HTTP client
    ├── webi_lib.h / webi_lib.c            # Native HTTP server, MIME table, path-traversal-safe file resolution
    ├── sqlite_lib.h / sqlite_lib.c        # SQL bridge — real libsqlite3, needs libsqlite3-dev
    ├── vision_lib.h / vision_lib.c        # Native image I/O + pixel ops (honest by design — see its own header comment)
    ├── vision_cv.h / vision_cv.c          # Filters, morphology, thresholding, transforms
    ├── vision_cascade.h / vision_cascade.c # Real Haar-cascade (Viola-Jones) face/object detection
    ├── ocr_lib.h / ocr_lib.c              # Native Tesseract bridge
    ├── stb_image.h / stb_image_write.h / stb_image_resize2.h  # Vendored (sean barrett's stb libs)
    ├── main.c                         # Entry point — wires every native library into the VM
    └── kh.c                           # Package manager CLI
```

---

## Building from Source

### Prerequisites

- **GCC** (C11) — MinGW64 on Windows, or any POSIX GCC
- **GNU Make**
- **libtesseract** (+ its trained language data) — required by the `ocr` package; the build fails without it rather than silently skipping OCR. See [docs/ocr.md](docs/ocr.md) for the exact install command per platform (apt/Homebrew/MSYS2) — on MSYS2 specifically, note that the language data is a *separate* package from the library itself.
- **libsqlite3** (dev headers) — required by the `sqlite` package's native bridge. `apt install libsqlite3-dev` / `brew install sqlite3` / MSYS2 `mingw-w64-x86_64-sqlite3`. The build fails without it (missing `sqlite3.h`) rather than silently skipping SQLite support.
- **Windows only**: links `-lwinhttp`, `-lshell32`, and `-lsqlite3` (the first two ship with Windows; sqlite3 does not)
- **Linux/macOS**: links `-lm` and `-lsqlite3`, uses `curl` for HTTP

### Build

```bash
git clone https://github.com/koshirfan1/Kosh.git
cd Kosh
make
```

Produces `kosh.exe` (the compiler+VM runtime) and `kh.exe` (package manager) on Windows, or `kosh` and `kh` on Linux/macOS.

```bash
make clean   # remove build artifacts
make install # Linux/macOS: install kosh and kh to ~/.kosh/bin (override with PREFIX=/your/path)
make uninstall # remove them again
```

On Windows, `make install`/`make uninstall` print a pointer to `install.ps1` instead of doing the copy themselves — see that script's own comments for why PATH persistence needs the registry (`[Environment]::SetEnvironmentVariable`), not something a makefile target can do portably.

The [Quick Start](#quick-start) section's `install.sh` (Linux/macOS) / `install.ps1` (Windows) does the `git clone` + `make` + `make install` sequence above for you, plus PATH setup — this section is the manual, step-by-step equivalent if you'd rather not pipe a script into a shell, or need to customize the build (e.g. `LLM=1` for the optional llama.cpp bridge — see the makefile).

### Exit Codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `64` | Wrong number of arguments |
| `65` | Parse/syntax error |
| `70` | Runtime error |
| `74` | I/O error (file not found) |

### CLI flags

| Flag | Meaning |
|---|---|
| `kosh script.kh [args...]` | Run a script; extra args are available inside it as the global array `argv` |
| `kosh` (no arguments) | Starts an interactive REPL — see below |
| `kosh --version` / `-v` | Print the version and exit |
| `kosh --help` / `-h` | Print usage and exit |

### Interactive REPL

Running `kosh` with no arguments — the same trigger `python` uses — starts an interactive session: type a line, see it run immediately, keep going.

```
$ kosh
Kosh 0.015.0 — interactive mode. Type 'exit' or press Ctrl+D to quit.
kosh> let x = 10
kosh> x * 5
50
kosh> fn square(n):
...     return n * n
...
kosh> square(7)
49
kosh> exit
```

A few things worth knowing about how it behaves:
- **Variables persist across lines** — one shared VM instance runs every line you type, so `let x = 10` on one line is still visible as `x` on the next, the whole point of a REPL over running one-off scripts.
- **A bare expression auto-prints its value** (`x * 5` above shows `50` with no explicit `print`) — same convention as Python's REPL. Statements that don't produce a value (`let`, `if`, `fn`, …) print nothing, same as always.
- **Multi-line blocks**: Kosh is indentation-based like Python, so a line ending in `:` (an `fn`/`if`/`while`/`try` header) switches the prompt to `...` and keeps reading indented lines until a **blank line** closes the block, then runs the whole thing at once. This is a simpler rule than Python's own indentation-tracking REPL, but it means the same thing in practice: finish a block, then hit Enter on an empty line.
- **A runtime error doesn't end the session** — it prints and the REPL keeps going with all your variables intact, the same way a real interactive interpreter should behave, rather than one mistake forcing a restart.
- Type `exit`, `quit` (with or without `()`), or press Ctrl+D to leave.

---

## Architecture

### Pipeline

```
Source .kh file
     │
     ▼
  Lexer  ──────────────── Token stream (INDENT/DEDENT/keywords/literals)
     │
     ▼
  Parser ──────────────── Abstract Syntax Tree (31 node types)
     │
     ▼
  Compiler ─────────────── Bytecode (38 opcodes) — see docs/opcodes.md
     │
     ▼
  VM ───────────────────── Stack-based execution — this is what actually runs
     │
     ▼
  Output / Side effects
```

A tree-walk interpreter (`interpreter.c`) exists in the source tree from an earlier design, but it is not the execution path above — it's retained only for one legacy import-parsing case. Every script you run goes through the compiler and VM.

### VM Design

- **Value system**: tagged union (`VAL_NUMBER`, `VAL_STRING`, `VAL_BOOL`, `VAL_NIL`, `VAL_ARRAY`, `VAL_MAP`, `VAL_FUNCTION`, `VAL_NATIVE`)
- **Execution model**: stack-based bytecode VM, 38 opcodes (see [docs/opcodes.md](docs/opcodes.md))
- **Closures**: value-capture only — a nested function can read an enclosing variable, but there's no shared mutable upvalue, so a "counter closure" pattern that mutates captured state doesn't work the way it does in Python/JS. Known limitation, not yet addressed.
- **Memory management**: reference-counted, plus a Bacon-Rajan trial-deletion cycle collector for arrays/maps (runs automatically, also callable as `gc_collect()`) — a circular reference no longer leaks for the process lifetime. Closures aren't covered by the collector yet (separate refcount) — see [docs/gc-notes.md](docs/gc-notes.md) for the full design and known limitation
- **Compiler optimizations**: constant folding is implemented; dead-code elimination, constant propagation, and peephole optimization are not yet

### Package Resolution

`import "name"` resolves in this order:
1. Relative to current script directory (e.g. `./name`)
2. Relative path as-is
3. `~/.kosh/packages/name/name.kh` (installed packages)

`from "name" import A, B, C` resolves each requested name against a
plain symbol, the package's own name (flattens the whole module), or a
sibling submodule file next to it (flattens that file's public names) —
see [docs/from-import.md](docs/from-import.md) for details, including
the privacy rule (`_`-prefixed names never get flattened) and the
closure-snapshot ordering constraint that applies to any multi-file
package.

---

## Roadmap

| Phase | Status |
|---|---|
| Lexer | ✅ Complete |
| Parser | ✅ Complete |
| Tree-walk interpreter (original design) | ✅ Complete, now legacy — superseded by the compiler + VM below |
| Bytecode compiler + VM | ✅ Complete — the actual execution engine (38 opcodes, see [docs/opcodes.md](docs/opcodes.md)) |
| Standard Library | ✅ Complete |
| Import/Module system, `from X import A, B, C` | ✅ Complete |
| Package manager (`kh`), 37-package registry | ✅ Complete |
| `webi` — routing, security (CSRF/rate-limit/API-key/CORS), templates, static files, `after()` hooks | ✅ Complete |
| `vision` — image I/O, filters, thresholding, morphology, real Haar-cascade face detection | ✅ Complete |
| `ocr` — Tesseract bridge: text, word boxes, orientation correction, searchable PDF, whitelisting, multi-language | ✅ Complete |
| Fuzz testing (parser), memory stress testing (10M-allocation scale), CI | ✅ Complete — CI gates on the full test suite (not just "does it compile"), including a 1000-iteration mutation-fuzzing run. **Windows-only for now** (was Linux/Windows/macOS — narrowed at the maintainer's request; the Linux/macOS steps are still in the workflow file, just inactive, so restoring them later is a one-line change). Running this against a real Windows runner for the first time found and fixed several genuine cross-platform bugs — a `strndup` portability issue, an MSYS2/PATH toolchain mismatch, and two CI-script-only bugs (bash's errexit swallowing an intentionally-nonzero exit code check, and MSYS2's shell not exposing Python) — see `ROADMAP_STATUS_UPDATED.md` item 17 for the full account. |
| `{}` map as a real hash table | ✅ Complete — open-addressing (FNV-1a), see [docs/hash-table-audit.md](docs/hash-table-audit.md) |
| String concatenation performance | 🟡 Partial — redundant rescan fixed (~2.4x), full O(n²) root cause still open, see [benchmarks/RESULTS.md](benchmarks/RESULTS.md) |
| Mutable closures (currently value-capture only) | 🔲 Planned |
| Garbage collector (circular references) | ✅ Complete — cycle collector for arrays/maps, see [docs/gc-notes.md](docs/gc-notes.md) |
| Error handling (`try`/`catch`/`throw`) | ✅ Complete, see [docs/trycatch-implementation.md](docs/trycatch-implementation.md) |
| Classes (`class`, `self`, `__init__`, methods, fields, inheritance, `super`) | ✅ v1 complete (single inheritance only), see [docs/classes.md](docs/classes.md) |
| Tensor + autograd (`Tensor`, `.backward()`, `.grad`, row/col broadcasting) | ✅ v1 complete (2-D-only matmul/transpose), see [docs/tensor.md](docs/tensor.md) |
| nn module (`Dense`, activations, `Sequential`, MSE/CrossEntropy loss, `SGD`/`Adam`) | ✅ v1 complete, see [docs/nn.md](docs/nn.md) |
| Dispatch-loop optimization (computed goto) | ✅ Tried, measured slower than the existing switch, reverted — see [docs/dispatch-perf.md](docs/dispatch-perf.md) |
| `webi` threaded server (thread-per-connection, concurrency cap) — see [docs/phase4-plan.md](docs/phase4-plan.md) | 🔲 Planned |
| ONNX Runtime bridge (run pretrained deep-learning OCR models) — see [docs/onnx-ocr-plan.md](docs/onnx-ocr-plan.md) | 🔲 Planned |
| Binary-safe string type (length-prefixed, not NUL-terminated) | 🔲 Planned |
| Self-hosted compiler | 🔲 Planned |
| **v1.0 release** | 🔲 Not yet — see `ROADMAP_STATUS_UPDATED.md` for the full, current criteria-by-criteria status |

---

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Build and test: `make && kosh examples/full_test.kh`
4. Commit and push
5. Open a Pull Request

To publish a new package, add a folder under `packages/` and update `packages/registry.json`.

---

## License

MIT License — open source, free to use.

---

## Author

**Irfan Khan** — Creator and maintainer of the Kosh programming language.

- GitHub: [@koshirfan1](https://github.com/koshirfan1)
- Repository: [github.com/koshirfan1/Kosh](https://github.com/koshirfan1/Kosh)

---

*Built from scratch in C — because understanding the fundamentals matters.*
