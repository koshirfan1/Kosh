# Packages and the `kh` package manager

[← Back to the README](../README.md)

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

## Packages

Every package installs the same way, regardless of category: `kh install <name>`, then `import "<name>"`.

<!-- PACKAGES-TABLE:BEGIN -->
38 packages in 7 categories.

### Core & CLI

| Package | Version | Description |
|---|---|---|
| `math` | 2.0.1 | Advanced math functions: sqrt, pow, gcd, primes, factorial, mean |
| `strings` | 1.0.0 | String utilities: split, trim, replace, pad, repeat, contains, join |
| `collections` | 1.0.0 | Functional array helpers: sort, filter, map_each, reduce, find, unique, chunk, zip, sort_by |
| `colors` | 1.0.0 | Terminal color output using ANSI codes: red, green, bold, boxes, etc. |
| `argparse` | 1.0.0 | Command-line argument parser |
| `validation` | 1.0.0 | Data validation utilities: emails, lengths, numeric checks |
| `dotenv` | 1.0.0 | Load environment variables from .env files |
| `uuid` | 1.0.1 | Unique ID generation: uuid_v4, short IDs, named entity IDs, sequential counters |
| `events` | 1.0.1 | Event emitter system: on, emit, once, off, history — built for Kosh Engine game events |
| `logger` | 1.0.1 | Structured logger: debug/info/warn/error levels, file output, silent mode |
| `test` | 1.0.0 | Unit testing framework: assertions, test suites, pass/fail reporting |
| `datetime` | 1.0.1 | Date and time utilities: timer, elapsed, human-readable duration, sleep helpers |

### Web: the webi ecosystem

| Package | Version | Description |
|---|---|---|
| [`webi`](webi.md) | 1.2.0 | Lightweight web framework: routing, requests, responses, middleware, HTTP server |
| `webi_auth` | 1.0.0 | Authentication middleware for Webi |
| `webi_socket` | 1.0.0 | WebSocket support for Webi framework |
| `morgos` | 1.0.0 | Morgan-style HTTP request logger for webi: colored status, elapsed time, response size — as a swappable after-hook |

### HTTP, APIs & testing

| Package | Version | Description |
|---|---|---|
| `requests` | 1.1.0 | HTTP client: get, post, post_json, put, delete with auto JSON decode |
| `postman` | 1.0.0 | API testing toolkit: send requests, assert responses, run test suites |
| `swagger` | 1.0.0 | Swagger UI and API documentation generator |
| `openai` | 2.0.0 | Production-grade OpenAI API client: chat completions, multi-turn conversations, embeddings, moderation, and model listing, with structured error handling and automatic retry/backoff. |

### Data & storage

| Package | Version | Description |
|---|---|---|
| `fs` | 1.0.1 | File system utilities: read, write, append, copy, path helpers, JSON config |
| `csv_io` | 1.0.0 | CSV file reader and writer |
| `json_db` | 1.0.0 | Simple NoSQL database using JSON files |
| `orm` | 1.0.0 | Simple Object-Relational Mapper for JSON databases |
| `sqlite` | 1.0.0 | SQL Database bridge for high-speed local data persistence *(needs libsqlite3 (dev headers) to build)* |

### Computer vision & OCR

| Package | Version | Description |
|---|---|---|
| `vision` | 3.0.0 | Full classical computer-vision toolkit: image I/O, filters (blur/sharpen/emboss/edges), thresholding (fixed/Otsu/adaptive), morphology, connected-component blob detection, histograms, drawing, template matching, HSV color-range detection, and real Haar-cascade face detection (Viola-Jones). All native/fast, no ML framework required. |
| [`ocr`](ocr.md) | 1.1.0 | Real OCR via Tesseract: text, word-level boxes, orientation correction, searchable PDF, char whitelist, multi-language *(opt-in: build with `make OCR=1` (needs libtesseract))* |

### AI / ML utilities

| Package | Version | Description |
|---|---|---|
| [`tensor`](tensor.md) | 1.0.0 | Multi-dimensional array math for AI |
| [`nn`](nn.md) | 1.0.0 | Minimal neural-network module on top of tensor: Dense, ReLU, Sequential, MSE/cross-entropy losses, SGD and Adam optimizers. Requires the tensor package. |
| `kbrain` | 1.0.0 | Simple machine learning and AI utilities |
| `nlp` | 1.0.0 | Natural Language Processing and Sentiment Analysis |
| `quant` | 1.0.0 | Accuracy-preserving quantization in pure Kosh: per-channel calibration, bias correction, outlier-aware sparse decomposition and mixed-precision strategies. |

### Networking & protocols

| Package | Version | Description |
|---|---|---|
| `dns` | 1.0.0 | DNS lookup utilities using DNS-over-HTTPS |
| `ftp` | 1.0.0 | FTP client for file transfers |
| `grpc` | 1.0.0 | gRPC client for high-performance RPC |
| `mqtt` | 1.0.0 | MQTT protocol for IoT messaging |
| `smtp` | 1.0.0 | Email sending utilities |
| `ssh_client` | 1.0.0 | SSH client for remote command execution |
<!-- PACKAGES-TABLE:END -->

Full detail on the packages below is in their own docs where noted; everything else is documented inline in its `.kh` source under `packages/<name>/`.

## Examples for a few core packages

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
