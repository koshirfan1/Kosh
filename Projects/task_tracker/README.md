# Task tracker - real SQLite, real sessions, real per-user data

A small but genuinely complete web app: `webi` (routes/server) + `sqlite`
(real persistence) + session-based per-user task ownership. Built to
exercise Khan as an actual web backend, not just isolated feature demos.

## A real bug found and fixed to build this

`sqlite_lib.c` was a mock: `sqlite_exec()` ignored the SQL string
entirely and always returned `true`, no matter what you passed it - no
data was ever actually stored. `sqlite.kh`'s own header even already
documented `sqlite_query`/`sqlite_close` as part of the intended API,
but neither was implemented. This wasn't a hypothetical concern - it was
found while building this project, by trying to persist data and reading
the actual `.c` file to see why nothing came back.

Fixed: `sqlite_lib.c` now links against real `libsqlite3` (`-lsqlite3`,
added to the default `LDFLAGS` - this is a new real dependency for the
default build, `libsqlite3-dev` at build time). All four functions
(`sqlite_open`/`sqlite_exec`/`sqlite_query`/`sqlite_close`) do real work:
verified by writing rows in one process, reading them back correctly in
a separate process invocation, and confirming the file on disk is a
genuine SQLite database (`file` command output, not just a Khan-internal
format).

## Parameter binding (this matters, not just style)

`sqlite_exec`/`sqlite_query` both take an optional third argument: an
array bound to the SQL's `?` placeholders via real `sqlite3_bind_*`
calls, not string concatenation. Every place this app touches SQL with a
value that came from a request (username, task title, task id) uses this
- never `"... WHERE x = '" + value + "'"`.

Tested directly: inserted the string `Robert'; DROP TABLE t; --` as a
bound parameter. It was stored as the literal string and the table
survived - confirmed both by reading the value back unchanged and by
checking `sqlite_master` afterward. String-concatenating that same value
into the SQL instead would have dropped the table; this is what
parameter binding is actually for, demonstrated, not just claimed.

## Auth - deliberately simple, stated plainly

Khan has no password-hashing primitive (no sha256/bcrypt native exists
anywhere in this codebase). Rather than fake security with something
that looks like a password check but isn't, this app has no passwords at
all: `POST /login` with any username creates that user the first time or
logs into the existing one after, and returns a real, server-checked
session token (verified against a `sessions` table on every subsequent
request). That means anyone who knows a username can act as that user -
fine for a local/demo tool, not fine for anything with real accounts.
Adding real auth needs a real hash primitive in Khan first, which this
project doesn't add (out of scope here - a Khan core change, not an app
concern).

## One honest API nuance

`POST /tasks/:id/toggle` and `DELETE /tasks/:id` both scope their SQL to
`WHERE id = ? AND user_id = ?`, so one user genuinely cannot affect
another's task even if they guess the id - verified directly (user B's
toggle attempt on user A's task returns `{"toggled": true}` but A's task
is confirmed unchanged afterward). The security boundary is real. What's
*not* precise: the response doesn't distinguish "your task was toggled"
from "no task matched you" - both return the same success shape, since
this doesn't check `sqlite3_changes()`. Worth knowing if you build on
this; the data isolation holds regardless.

## One more real bug, found while building the frontend

The `datetime` package's own comment claims `clock()` returns "seconds
since epoch," and `dt_now()`/`dt_format()` are built on that assumption.
It doesn't - Khan's native `clock()` is the C standard library's
CPU-time clock, not wall-clock time. Confirmed directly:
`dt_format(dt_now())` returns `"00:00:00"`, always, regardless of when
it's actually called. Not fixed here (a `datetime` package problem, not
a task-tracker one), but it's why this app's `created_at` column exists
in the database but is deliberately not shown in the UI - a fake
timestamp would be worse than no timestamp. The task's own position in
the ledger (`id`, shown as the index number) is the real, meaningful
order.

## Frontend

A real static frontend now lives in `public/` (`index.html`, `style.css`,
`app.js`) - no framework, no build step, served straight by `webi`'s
`serve_static()`. Design: dark ink-navy, a brass accent, IBM Plex Mono
paired with IBM Plex Sans, tasks laid out as numbered ledger lines rather
than generic cards/checkboxes - completing one plays a small stamp-in
animation instead of a plain checkbox tick.

Two path-resolution conventions collided while wiring this up, found by
actually testing rather than assumed:
- `serve_static(app, "/static", "public")`'s folder argument resolves
  relative to *this .kh file's own directory* - works regardless of
  where you run `khan` from.
- `read_file()` (used in `h_index` to load `index.html`) resolves
  relative to the *process's working directory* instead - same
  convention as `llm_load()` in the LLM bridge, different from
  `serve_static()`. Run `khan` from the repo root, as below; that's the
  path `read_file()` here is written for.

## Run it

```bash
./khan examples/task_tracker/app.kh
# then open http://localhost:8200 in a browser
```

Requires `libsqlite3-dev` at build time (new default-build dependency -
see the makefile diff). Data persists in `tasks.db` next to wherever you
run `khan` from.
