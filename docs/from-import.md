# `from X import A, B, C`

Pulls one or more names out of a module or package into the current
scope, without bringing in everything it defines. Each name in the list
is resolved independently, in order, against three possibilities:

## 1. A plain symbol

```kosh
from webi import webi_app, webi_version

let app = webi_app()
print webi_version()
```

Works exactly like importing the whole module and then only using two of
its names — `webi_app`/`webi_version` are functions the `webi` package
defines directly.

## 2. The package's own name

```kosh
from webi import webi
```

Equivalent to a plain `import "webi"`, just reached through the
`from`-import syntax — every public name the package defines gets pulled
into your scope.

## 3. A sibling submodule file

```kosh
from webi import security
```

If `security` isn't a plain symbol, Kosh checks whether `security.kh`
exists next to the file being imported from (`webi.kh`'s own directory).
If it does, that file is parsed and run, and its public functions are
flattened into your scope — so you get everything `security.kh` defines
(`csrf_new_token`, `webi_set_cors_origins`, ...) without a separate
install or a separate `import "webi/security.kh"` statement.

## All three combined

```kosh
from webi import webi, security, requests, json

let app = webi_app()               # from webi.kh itself
let token = csrf_new_token()       # from security.kh
let res = get("https://...")       # from requests.kh
let s = encode({"a": 1})           # from json.kh
```

One line pulls in the whole framework plus its two optional thin
re-export submodules (`requests.kh`/`json.kh`, native `http_*`/`json_*`
builtins under friendlier names) plus the security helpers.

---

## Privacy

Only names **without** a leading underscore get pulled in by cases 2 and
3 above. Internal helpers like `_webi_dispatch` or `_match_route` stay
private even when you flatten-import the whole package or a submodule:

```kosh
from webi import security

csrf_new_token()             # OK — public
_webi_strip_bearer_prefix()  # Runtime error: Undefined function.
```

This only applies to the flatten-import paths (self-name and submodule);
it doesn't change what a Kosh file can see internally — sibling files
inside the same package can still call each other's private helpers
normally, the same way they always could.

## Errors

A name that's neither a plain symbol, the package's own name, nor a
sibling submodule file still gets a clear error instead of silently
doing nothing:

```kosh
from webi import totally_not_a_thing
```
```
[line 0] Runtime error: cannot import name 'totally_not_a_thing' from 'webi' (...)
```

## A note on ordering, if you're writing a package that uses this

This interpreter's closures snapshot the enclosing scope **at
declaration time** — a function can only call a helper that was already
defined earlier in the same file (or in a file imported earlier). There
are no forward references. If you split a package into multiple files
the way `webi` does, order your `import` statements in the aggregate
file (`webi.kh`) so anything a later file depends on is imported first —
see the `IMPORT ORDER MATTERS` comment at the top of `packages/webi/webi.kh`
for a concrete example of what breaks if you get this wrong.
