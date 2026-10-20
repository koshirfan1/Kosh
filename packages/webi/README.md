# webi — Kosh's web framework

`webi` is Kosh's built-in-style web framework: routing, request/response
helpers, middleware, opt-in security (CSRF, rate limiting, API keys, CORS),
HTML templates, static file serving, and a small HTTP server — all written
in Kosh itself (`packages/webi/*.kh`) on top of a handful of native
C helpers (`src/webi_lib.c`) for the parts that genuinely need them
(sockets, MIME types, path safety, crypto-random tokens).

```bash
kh install webi
```

```kosh
import "webi"

let app = webi_app()
app = route(app, "GET", "/", fn_index)

fn fn_index(req):
    return res_html("<h1>Welcome to webi!</h1>")

webi_run(app, 8080)
```

Or pull in the security/HTTP-client/JSON helpers in the same line with
[`from`-import](from-import.md):

```kosh
from webi import webi, security, requests, json
```

---

## Table of contents

- [App & routing](#app--routing)
- [Reading a request](#reading-a-request)
- [Building a response](#building-a-response)
- [Middleware](#middleware)
- [Security](#security)
- [Templates](#templates)
- [Static files](#static-files)
- [Running the server](#running-the-server)
- [Known limitations](#known-limitations)

---

## App & routing

```kosh
let app = webi_app()
```

Returns a fresh app context — a plain map (`routes`, `middlewares`, `host`,
`port`, `debug`, `not_found`, `error`, `name`). Every `webi_*`/`route`/`use`
function takes an app map and returns a new one; reassign as you go:

```kosh
app = webi_debug(app, true)      # verbose per-request logging
app = webi_name(app, "my-api")   # shown in logs
```

`webi_debug(app, true)` also turns on an automatic, Morgan-style request
log — no middleware needed — printed once per request straight from
`webi_handle()` after the response is built:

```
[webi] GET /users/42 200 12.48 ms - 1024
```

Method, path, status (color-coded — green 2xx, cyan 3xx, yellow 4xx, red
5xx), elapsed time covering the full request (middleware + routing +
handler), and response body size in bytes. This is separate from
`mw_logger` below, which is an opt-in pre-request middleware with no
status/timing (it runs before the response exists) — turn on
`webi_debug()` if you want the Morgan-style summary line, add
`mw_logger` as well if you also want to see a request logged the moment
it arrives, before the handler runs.

Register routes with `route()`, or the per-method shortcuts:

```kosh
app = route(app, "GET", "/", fn_index)
app = get_route(app, "/users/:id", fn_get_user)
app = post_route(app, "/users", fn_create_user)
app = put_route(app, "/users/:id", fn_update_user)
app = patch_route(app, "/users/:id", fn_patch_user)
app = delete_route(app, "/users/:id", fn_delete_user)
```

Path patterns support:
- **Static segments** — `/users`
- **Params** — `:id` → available via `req["params"]["id"]` or `param_get(req, "id", default)`
- **Wildcards** — `/*` matches any remaining path (used internally by `serve_static()`)

Custom error handlers:

```kosh
app = webi_not_found(app, fn (req): return res_text("nothing here"))
app = webi_error(app, fn (req): return res_text("something broke"))
```

---

## Reading a request

Every handler receives one `req` map:

| Field | Description |
|---|---|
| `req["method"]` | `"GET"`, `"POST"`, ... |
| `req["path"]` | Request path, e.g. `"/users/42"` |
| `req["query"]` | Parsed query-string map |
| `req["headers"]` | Header map |
| `req["body"]` | Raw request body string |
| `req["body_json"]` | Body parsed as JSON (or `nil` if it isn't valid JSON) |
| `req["params"]` | Route param values (from `:name` segments) |
| `req["ip"]` | Client IP |

Helpers instead of indexing directly (each takes a fallback default):

```kosh
let name  = query_get(req, "name", "World")     # ?name=...
let id    = param_get(req, "id", nil)           # :id route param
let auth  = header_get(req, "Authorization", "")
let token = cookie_get(req, "session", "")
let email = form_get(req, "email", "")          # url-encoded form body

let data = body_json(req)                       # parsed JSON body, or nil

if is_get(req): ...
if is_post(req): ...
if is_json_req(req): ...    # Content-Type: application/json
```

---

## Building a response

Every handler returns a response map — build one with `res_*`:

```kosh
return res_text("plain text")
return res_html("<h1>hi</h1>")
return res_json({"ok": true})
return res_json_str(already_encoded_json_string)
return res_status(418, "I'm a teapot")

return res_redirect("/login")
return res_redirect_permanent("/new-url")

return res_not_found("nothing here")        # 404
return res_bad_request("missing field")     # 400
return res_unauthorized("log in first")     # 401
return res_forbidden("nope")                # 403
return res_server_error("oops")             # 500
return res_created({"id": 1})               # 201
return res_no_content()                     # 204
```

Chain header/cookie helpers onto any response:

```kosh
let res = res_json({"ok": true})
res = res_with_header(res, "X-Request-Id", req_id)
res = res_with_cookie(res, "theme", "dark")
res = res_with_secure_cookie(res, "session", token, {"http_only": true, "max_age": 3600})
res = res_cors(res, req)   # apply your configured CORS origin to this one response
```

---

## Middleware

```kosh
app = use(app, mw_logger)
```

A middleware is a function `fn(req) -> req` that runs before the matched
route handler, in registration order. It can:
- Return a modified `req` (add a field, e.g. `_app`)
- Short-circuit the whole request by setting `req["_webi_short_circuit"]`
  to a response map — the route handler never runs, and that response is
  sent as-is. This is how CORS preflight, CSRF rejection, rate limiting,
  and API-key rejection all work.

Built-ins:

```kosh
app = use(app, mw_logger)      # print "[webi] GET /path" as each request comes in
app = use(app, mw_cors)        # reflect your configured CORS origin, handle OPTIONS preflight
app = use(app, mw_json_only)   # reject non-JSON requests with 415
```

Write your own the same way:

```kosh
fn mw_request_id(req):
    req["request_id"] = uuid_v4()
    return req

app = use(app, mw_request_id)
```

### After-hooks — the other end of the request (v1.1.3)

Middleware only ever sees `req`, before the handler runs — it can't
read or react to the response. **After-hooks** are the opposite:
`fn(req, res) -> res`, run once the handler has already produced a
response, in registration order, each one seeing the previous hook's
result.

```kosh
fn hook_add_header(req, res):
    res = res_with_header(res, "X-Request-Id", req["request_id"])
    return res

app = after(app, hook_add_header)
```

This is what makes real post-response logging — status code, elapsed
time, response size, the things Node's `morgan` logs — possible as an
ordinary installable package instead of something wired into webi's
core. `req["_start_clock"]` (a `clock()` reading taken before
middleware/dispatch even run) is stamped onto every request specifically
so an after-hook can compute elapsed time without webi needing to know
anything about logging.

**`morgos`** is exactly that — a separate package, one function:

```bash
kh install morgos
```

```kosh
from webi import webi
import "morgos"

let app = webi_app()
app = after(app, morgos)
```

```
GET /users/42 200 12.48 ms - 1024
```

Unlike `webi_debug()`'s built-in logging, `morgos` logs every request
unconditionally (no debug flag needed), and — being an ordinary
after-hook — can be swapped out, combined with another after-hook (a
file logger via the `logger` package, say), or removed, without
touching webi itself.

---

## Security

Everything here is **opt-in** — nothing runs unless you wire it up with
`use()`.

### CORS

```kosh
app = webi_set_cors_origins(app, ["https://example.com"])  # default: ["*"]
app = use(app, mw_cors)
```

### Rate limiting

```kosh
app = webi_set_rate_limit(app, 100, 60)   # 100 requests / IP / 60s window
app = use(app, mw_rate_limit)             # 429 once exceeded
```

Counting happens in native code (`rate_limit_check()`), since a handler
only ever sees a fresh copy of `app` — counts can't live on the Kosh map
and survive across requests.

### API-key auth

```kosh
app = webi_require_api_key(app, ["secret-key-1", "secret-key-2"])
app = use(app, mw_require_api_key)   # checks Authorization: Bearer <key>
```

### CSRF

```kosh
fn fn_form(req):
    let token = csrf_new_token()
    let html = render("...<input type='hidden' name='csrf_token' value='{{t}}'>...", {"t": token})
    return csrf_issue(res_html(html), token)   # sets the token as an HttpOnly cookie

app = use(app, mw_csrf)   # verifies the cookie against the submitted token on POST/PUT/PATCH/DELETE
```

`csrf_new_token()` uses `secure_token()` — an OS-CSPRNG-backed random
token (`/dev/urandom` on POSIX, `CryptGenRandom` on Windows), not Kosh's
`random()`, which is seeded `rand()` and predictable enough to never use
for anything security-sensitive.

---

## Templates

```kosh
let html = render("<h1>Hello, {{name}}!</h1>", {"name": "Irfan"})
```

`{{key}}` placeholders are replaced with `str(vars[key])`.

To read a template off disk instead of inlining the string:

```kosh
let html = render_file("templates/index.html", {"name": "Irfan"})
if html == nil:
    return res_server_error("template error")
return res_html(html)
```

`render_file(path, vars)`:
- Resolves `path` relative to whichever `.kh` file is calling it (same
  directory-aware resolution `import` uses), not the process's current
  directory.
- Rejects anything that would escape that directory — a `..` segment
  (raw or percent-encoded), an absolute path, or (on POSIX) a symlink
  pointing outside it. This matters if `path` is ever built from request
  input (e.g. `"templates/" + req["query"]["page"] + ".html"`).
- Only accepts `.html`, `.htm`, `.txt`, `.xml`, `.svg` — there's no
  legitimate reason for a template renderer to open anything else.
- Caps files at 2 MB (a sanity backstop, not a real limit).
- Returns `nil` on any rejection instead of raising, so a broken template
  path doesn't take the whole app down — always check for `nil`.

`html_escape(s)` escapes `&`, `<`, `>`, `"`, `'` — use it on any
user-supplied value you interpolate into HTML to avoid XSS:

```kosh
let safe_name = html_escape(req["query"]["name"])
```

---

## Static files

```kosh
app = serve_static(app, "/static", "public")
```

Mounts the `public` folder (resolved relative to the calling script, same
as `render_file()`) so `GET /static/style.css` returns
`public/style.css`, with `Content-Type` set from a small built-in MIME
table (html/css/js/json/svg/png/jpg/gif/ico/webp/woff/ttf/pdf/mp4/mp3/...;
unknown extensions get `application/octet-stream`).

The part of the URL *after* the mount prefix is re-validated on **every
request** — not just once at mount time — the same way `render_file()`
validates its path: the resolved file must stay inside the mounted
folder, whether the escape attempt is a literal `..`, a percent-encoded
`%2e%2e%2f`, or (on POSIX) a symlink pointing outside it. This is the
same class of bug as the classic `GET /static/../../server.kh` attack.

```kosh
app = serve_static(app, "/static", "public")
app = serve_static(app, "/assets", "vendor/assets")   # mount as many as you need
```

---

## Running the server

```kosh
webi_run(app, 8080)              # binds 0.0.0.0:8080
webi_run_host(app, "127.0.0.1", 8080)   # bind a specific host
```

Both block forever, dispatching one request at a time through
`webi_handle()` — the same bridge function the native C accept loop
calls per connection. You normally don't call `webi_handle()` yourself;
it's exposed mainly so tests can dispatch a request without actually
opening a socket (see `examples/webi_phase3_test.kh` for an example).

```kosh
print webi_version()    # "webi/1.1.3 (Khan)"
print webi_routes(app)  # list every registered route, for debugging
```

---

## Known limitations

- **Single-threaded server.** `http_serve()` handles one connection fully
  (read → dispatch → respond → close) before accepting the next. A slow
  client currently blocks every other visitor. A threaded version is
  planned but not yet built — see the project's roadmap; it needs either
  a global lock around request dispatch (simplest), a worker-process
  pool, or a fully thread-safe interpreter, since nothing in
  `interpreter.c` is locked today.
- **Strings are C-strings, not byte buffers.** `read_file()`/response
  bodies rely on `strlen()`/NUL-termination under the hood, so a file
  containing an embedded NUL byte truncates at that byte. Text assets
  (html/css/js/svg/json/txt) essentially never contain one; many
  real-world binary formats (png/jpg/etc.) do somewhere in the file and
  can come back truncated through `serve_static()`. Fine for text-based
  static assets; true binary-safe serving would need Kosh's string type
  to carry an explicit length instead of relying on NUL-termination.
- **No per-app persistent state across requests.** Each request gets a
  fresh copy of `app` — that's why rate limiting counts live in native
  code rather than on the Kosh map. If you need shared mutable state
  (a request counter, an in-memory cache), it has to live outside Kosh's
  normal value semantics for now.
