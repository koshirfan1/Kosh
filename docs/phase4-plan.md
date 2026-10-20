# Phase 4 plan — threaded server

Status: **planning only, not yet implemented.** This document is the
design; implementation is a separate step once this is agreed on.

## The problem, precisely

`fn_http_serve()` in `src/webi_lib.c` (both the POSIX version around
line 592 and the Windows version around line 876) is a single `while(1)`
loop: `accept()` → read request → parse → call `webi_handle()` → build
response → `send()` → `close()` → back to `accept()`. One connection is
handled start-to-finish before the next is even accepted. A client that
connects and sends data slowly (or a handler that's genuinely slow —
a big template render, a synchronous outbound HTTP call from inside a
route) blocks every other visitor for the duration, even though
`WB_SOCKET_TIMEOUT_SEC` (30s) already bounds the worst case of a client
that never sends anything at all.

## What actually needs protecting, and why it's more specific than "the interpreter isn't thread-safe"

I went and read the exact shared state involved before deciding on an
approach, rather than assuming. It's worse than "some maps might race" —
it's structural:

- There is exactly **one `Interpreter` struct**, created once in
  `main.c`, passed by pointer everywhere. Its fields aren't just
  "shared data" — they're the interpreter's **control-flow signaling**:
  `is_returning`, `return_value`, `is_breaking`, `is_continuing`,
  `had_runtime_error`, and `current_import_dir` (see `src/interpreter.h`
  lines 119–142). Every `return`, every loop `break`/`continue`, every
  runtime error, and even path resolution during `import` communicates
  through these single global fields, not through anything per-call.
- `khan_call_fn()` (called once per request, from `wb_safe_call_webi_handle`
  in `webi_lib.c`) drives `execute_block()` against this same shared
  `Interpreter*`. Two requests dispatched at the same wall-clock moment
  would have their `return`/`break` signals and error flags stomp on each
  other — this isn't a "might corrupt a map under heavy load" risk, it's
  "the second request's `return` can flip a flag the first request is
  still reading," full stop, deterministically wrong, not just a rare race.
- On top of that, `env_new`/`env_define`/`env_get` (environment
  chains) and map/array mutation in `khan_stdlib.c` do plain `malloc`
  with no locking either — real memory-corruption risk on top of the
  control-flow problem above.

This is why the plan explicitly rules out anything resembling "just
add threads and see" — the very first field two concurrent requests
would touch is shared, global, and unprotected.

## Chosen approach: (a) thread-per-connection + a single global dispatch lock

Confirming the recommendation from the original three-way writeup, now
with the concrete reason it's not just "the safe choice" but close to
the *only* structurally sound choice without a much bigger refactor:

- **(a) Global lock around dispatch** — cheap to implement correctly,
  because the critical section is exactly "the part that touches the
  shared `Interpreter*`," which is already a single, identifiable call
  (`wb_safe_call_webi_handle`). Everything else — `accept()`, reading
  the socket, parsing the HTTP request line/headers/body, building and
  `send()`-ing the response, `close()` — is per-connection local state
  and can run fully in parallel. This gets the actual availability win
  (a slow client's *I/O* doesn't block anyone else) without touching
  the interpreter's internals at all.
- **(b) Process-pool** — would sidestep the shared-`Interpreter*`
  problem entirely (each process gets its own), but the codebase has no
  IPC/fork infrastructure today, Windows doesn't have `fork()` (would
  need a fundamentally different implementation there vs POSIX, not
  just an `#ifdef` swap like the existing socket code), and since app
  state already doesn't persist across requests (each request gets a
  fresh copy of `app` per `webi_run`'s calling convention), there's
  nothing to lose by not sharing memory. Viable, but bigger and
  Windows/POSIX would diverge more than anything else in this codebase
  currently does.
- **(c) Fully thread-safe interpreter** — given what's above, this
  isn't "add mutexes to `env_get`/map mutation," it's "make
  `is_returning`/`return_value`/`is_breaking`/`is_continuing`/
  `had_runtime_error`/`current_import_dir` per-call-stack instead of
  global-singleton fields" — a change to the core `Interpreter` struct
  and every function that reads/writes those fields throughout
  `interpreter.c`. That's a correctness-critical refactor to the
  language runtime itself, not a webi-specific change, and it's not
  something to do without dedicated test time separate from a "web
  framework" phase.

Going with **(a)**.

## Concrete implementation plan

### 1. Threading primitives (`src/webi_lib.c`)

Match the existing `#ifdef _WIN32` / POSIX split already used for
sockets in this file, rather than introducing a new abstraction layer:

```c
#ifdef _WIN32
    typedef HANDLE            wb_thread_t;
    typedef CRITICAL_SECTION  wb_mutex_t;
    typedef HANDLE            wb_sem_t;
#else
    #include <pthread.h>
    #include <semaphore.h>
    typedef pthread_t   wb_thread_t;
    typedef pthread_mutex_t wb_mutex_t;
    typedef sem_t        wb_sem_t;
#endif
```

Small wrapper functions (`wb_mutex_init/lock/unlock`, `wb_sem_init/wait/post`,
`wb_thread_spawn_detached`) so the accept loop itself doesn't need
`#ifdef`s sprinkled through it — same style already used for
`wb_realpath()` in Phase 3.

**Build:** add `-lpthread` to the POSIX branch of `LDFLAGS` in the
`makefile`. (Harmless no-op on glibc ≥2.34 where pthread is merged into
libc, required on older glibc/musl — safe to always link.)

### 2. Global dispatch lock

```c
static wb_mutex_t g_webi_dispatch_lock;   // guards the Interpreter* itself
```

Initialized once in `fn_http_serve`/`fn_http_serve_workers` before
entering the accept loop. `wb_safe_call_webi_handle()` becomes the
critical section:

```c
wb_mutex_lock(&g_webi_dispatch_lock);
Value res_map = wb_safe_call_webi_handle(interp, env, call_args, argc, method, path);
wb_mutex_unlock(&g_webi_dispatch_lock);
```

Everything currently in the accept loop *outside* that call (socket
read, HTTP parsing, `wb_build_http_response`, `send`, `close`) stays
unlocked — that's the whole point.

### 3. Extract per-connection work into a thread function

Refactor the body of the current `while(1)` loop (everything from
`setsockopt(..., SO_RCVTIMEO, ...)` through `close(client_fd)`, lines
~646–710 POSIX / the Windows equivalent) into:

```c
typedef struct {
    int client_fd;              // or SOCKET on Windows
    struct sockaddr_in client_addr;
    Interpreter *interp;
    Environment *env;
    Value app;                  // already a value_copy() per existing code
} wb_conn_ctx_t;

static void *wb_handle_connection(void *arg) {
    wb_conn_ctx_t *ctx = (wb_conn_ctx_t *)arg;
    // ... exactly what the loop body does today, unchanged, down to
    // wb_read_request/wb_parse_request_line/wb_read_body/
    // wb_build_http_response/send/close ...
    // dispatch call wrapped in g_webi_dispatch_lock as above
    value_free(ctx->app);
    free(ctx);
    wb_sem_post(&g_webi_conn_slots);   // see concurrency cap below
    return NULL;
}
```

The accept loop becomes:

```c
while (1) {
    int client_fd = accept(server_fd, ...);
    if (client_fd < 0) { if (errno == EINTR) continue; break; }

    wb_sem_wait(&g_webi_conn_slots);   // blocks here if at the cap — see below

    wb_conn_ctx_t *ctx = malloc(sizeof(*ctx));
    ctx->client_fd = client_fd;
    ctx->client_addr = client_addr;
    ctx->interp = interp;
    ctx->env = g_webi_env;
    ctx->app = value_copy(app);        // each connection gets its own copy, same as today

    wb_thread_spawn_detached(wb_handle_connection, ctx);
}
```

Threads are **detached**, not joined — the server runs forever, and a
finished connection's thread just exits; nothing needs to wait on it.
If `wb_thread_spawn_detached` itself fails (resource exhaustion —
`pthread_create` returning `EAGAIN`, or `CreateThread` returning NULL),
fall back to handling that one connection synchronously right there in
the accept loop rather than dropping it silently or crashing — a
degraded-but-correct response beats no response.

### 4. Concurrency cap

A counting semaphore (`g_webi_conn_slots`, initialized to
`WB_MAX_CONCURRENT_CONNS`, default 64) acquired by the accept loop
*before* spawning a thread, and released by the thread when it finishes
(shown above). This means:

- Up to 64 connections are handled fully in parallel (I/O-wise; still
  serialized one-at-a-time through the dispatch lock for the actual
  Kosh execution).
- The 65th connection simply isn't `accept()`-ed yet — it sits in the
  kernel's listen backlog (already `listen(server_fd, 64)`) until a
  slot frees, rather than spawning thread #65 and letting the process
  run out of memory/threads under a connection burst. This is
  deliberately a *backpressure* mechanism (slow down accepting), not a
  *rejection* mechanism (send back a 503) — simpler, and consistent
  with not wanting to special-case "too busy" responses in this phase.
- This is a distinct mechanism from `webi_set_rate_limit()`/
  `mw_rate_limit` (Phase 2 security) — that one limits *requests per IP*
  over time; this one limits *total simultaneous connections* regardless
  of source, purely to bound memory/thread usage.

### 5. Kosh-level API surface

Keep `webi_run(app, port)` / `webi_run_host(app, host, port)` exactly as
they are today — no behavior change from the app author's perspective;
their existing apps get the threading transparently. Kosh doesn't
support default/optional parameters (confirmed: `fn_declaration` in
`parser.c` has no default-value syntax, and `khan_call_fn` enforces
strict arity), so rather than trying to bolt an optional `workers`/
`max_conns` parameter onto the existing functions, add new explicit
variants the same way `webi_run_host` already sits alongside
`webi_run`:

```kosh
webi_run(app, 8080)                          # unchanged, uses the default cap
webi_run_host(app, "127.0.0.1", 8080)        # unchanged, uses the default cap
webi_run_max_conns(app, 8080, 128)           # override the concurrency cap
webi_run_host_max_conns(app, "127.0.0.1", 8080, 128)
```

(Naming open to bikeshedding — the point is: explicit new functions,
not optional args, matching how this codebase already does it.)

### 6. What does *not* change

- `wb_safe_call_webi_handle`'s crash-containment behavior (catch a Kosh
  runtime error, log it, return a 500, keep serving) — unchanged, just
  now happens inside the lock instead of the single-threaded loop body.
  A runtime error in one request still can't take down the process, and
  now it *especially* can't take down other requests running
  concurrently in their I/O phase.
- `WB_SOCKET_TIMEOUT_SEC` / `SO_RCVTIMEO` — unchanged, still applies
  per-socket, now just relevant to each thread's own connection rather
  than the whole server.
- `WB_MAX_HEADER_SIZE` / `WB_MAX_BODY_SIZE` — unchanged.
- Route handlers, middleware, security, templates, static files — zero
  Kosh-level code changes. This is purely a native-layer change to how
  connections are accepted and dispatched.

## Testing plan

1. **Availability under a slow client** — open a raw TCP connection to
   the server and deliberately send the request line one byte at a time
   with delays (or just don't send anything, relying on the existing
   30s timeout), while a second, normal `curl` request runs concurrently
   against the same server. Before this phase: the second request
   blocks until the first times out. After: the second request completes
   immediately. This is the core regression test proving the phase did
   what it was for.
2. **Correctness under concurrency** — fire N concurrent requests
   (`curl` in a loop with `&`, or a small script) at a route that
   returns something request-specific (e.g. echoes a query param back),
   and diff every response against what was requested. This is the test
   that would catch a dispatch-lock bug (e.g. forgetting to lock one of
   the two `fn_http_serve` implementations, or a copy/paste gap between
   the POSIX and Windows versions) — any cross-talk between concurrent
   requests' responses fails immediately and obviously.
3. **Concurrency cap holds under a burst** — open more simultaneous
   connections than `WB_MAX_CONCURRENT_CONNS`, confirm the process's
   thread count never exceeds the cap (`ps -eLf` / Task Manager thread
   count) and that excess connections still eventually complete rather
   than erroring out.
4. **Existing suites still pass** — `webi_security_test.kh`,
   `webi_from_import_test.kh`, `webi_phase3_test.kh` all dispatch
   through `webi_handle()` directly (not through `http_serve()`), so
   they're testing Kosh-level logic that doesn't change in this phase —
   they should need zero changes and should keep passing as a sanity
   check that nothing above the native accept-loop layer was disturbed.
5. **Stress/fuzz pass** — run a longer burst (thousands of requests
   across dozens of concurrent connections) against a route that does
   real work (a template render, a map mutation) and watch for any
   crash, hang, or corrupted response over an extended run — this is
   the test most likely to surface a subtle lock-scope bug (locking too
   little, letting some interpreter-touching code slip outside the
   critical section) that a handful of manual `curl` calls wouldn't
   catch.

## Rollout

1. Implement the POSIX side first (thread function extraction, mutex,
   semaphore, accept-loop changes) and get it fully working and tested
   there, since that's the environment this gets validated in during
   development.
2. Mirror the same structure into the Windows `fn_http_serve`
   (`CreateThread`/`CRITICAL_SECTION`/semaphore via `CreateSemaphore`) —
   deliberately a straight port of the same design, not a
   reimplementation, to avoid the two platforms drifting into different
   behavior (already a discipline this file follows for the socket
   code).
3. Add the new `webi_run_max_conns`/`webi_run_host_max_conns` Kosh
   functions to `server.kh`.
4. Run the full existing test suite, then the new concurrency tests
   above (on the platform being developed on; ask for a Windows test
   pass the same way Phase 3 got validated there).
5. Update `docs/webi.md`'s **Known limitations** section — remove the
   single-threaded-server bullet, replace it with a short note on the
   concurrency cap and how to override it, and add a line about the
   dispatch lock meaning Kosh execution itself is still one-at-a-time
   even though I/O is now parallel (sets correct expectations — this
   is concurrent I/O, not concurrent computation).

## Explicitly out of scope for this phase

- A fully thread-safe interpreter (option c) — would need
  `is_returning`/`return_value`/`is_breaking`/`is_continuing`/
  `had_runtime_error`/`current_import_dir` moved off the singleton
  `Interpreter` struct onto something per-call, which is a
  language-runtime change, not a webi change.
- A process-pool/worker model (option b) — no fork()-equivalent
  abstraction exists in this codebase yet, and it would need real
  Windows/POSIX divergence rather than the `#ifdef`-level split used
  everywhere else here.
- Per-IP or per-route concurrency limits (only a global cap, matching
  what's described above) — could layer on top of the semaphore
  approach later if needed, but adds complexity this phase doesn't need.
- Graceful shutdown / connection draining (`Ctrl+C` currently just
  kills the process; detached threads mid-request would be abruptly
  terminated too) — not addressed here, flagging it as a known gap this
  phase doesn't fix.
