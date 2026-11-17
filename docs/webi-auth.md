# webi_auth: signed, expiring bearer tokens

`webi_auth` 2.0.0 gives a Webi app logins that work like this: after *you* have checked a
password, you hand the user a **token**; the client sends it back as
`Authorization: Bearer <token>`; `mw_auth` accepts the request only if the token is genuine
and has not expired.

```kosh
import "webi_auth"

let app = webi_app()
# once, at start-up. The secret is 32+ characters and lives OUTSIDE the source:
#   print auth_generate_secret()      # make one, then:  export MYAPP_AUTH_SECRET=<it>
app = webi_auth_secret_from_env(app, "MYAPP_AUTH_SECRET", {"ttl_seconds": 900})

fn login(req):
    let user = check_password(req)              # your code: password_verify(...) against your user store
    if user == nil:
        return res_unauthorized("bad credentials")
    return res_json({"token": auth_issue(req["_app"], user["id"])})

fn me(req):
    return res_json({"user": auth_user(req)})

app = route(app, "POST", "/login", login)
app = route(app, "GET", "/me", me)
app = webi_auth_public(app, ["/login"])         # the login route itself needs no token
app = use(app, mw_auth)                         # everything else needs a valid token
webi_run(app, 8080)
```

`webi_auth_public()` takes exact paths (`"/login/"` and `"/loginx"` are *not* `"/login"`).

## The token

```
kt1~<user>~<expires>~<nonce>~<mac>
```

`<mac>` is HMAC-SHA256 over everything before it, keyed with your secret. Changing the user or
the expiry, extending a token, or making one up needs the secret. `<expires>` is Unix seconds;
`<nonce>` is 64 random bits from the system CSPRNG, so two tokens for one user always differ.
User ids are 1-128 characters from `A-Z a-z 0-9 _ . @ : + -`.

## Functions

| Function | What it does |
|---|---|
| `auth_generate_secret()` | A fresh 64-hex-character secret. |
| `webi_auth_configure(app, secret, opts)` | Stores the secret on the app. `opts`: `ttl_seconds` (default 3600, max 30 days), `previous_secrets` (list). A secret under 32 characters is refused. |
| `webi_auth_secret_from_env(app, "VAR", opts)` | Same, reading the secret from an environment variable. Unset or short: auth stays unconfigured. |
| `auth_issue(app, user_id)` | A token for `user_id`, using the app's secret and default lifetime. |
| `auth_create_token(user_id, secret, ttl_seconds)` | Same without an app. Returns `nil` (and prints why) for a weak secret, a bad ttl or a bad user id. |
| `auth_verify_token(token, secret)` | `{"ok", "reason", "user", "expires", "id"}`. `secret` may be a list (current first). `reason` is `malformed`, `bad_signature` or `expired`: for your logs, never for the client. |
| `webi_auth_public(app, paths)` | Exact paths `mw_auth` lets through without a token (your login route). |
| `mw_auth` | Middleware: valid `Bearer` token or a plain 401. Sets `req["user"]` and `req["auth"]`. With no secret configured it rejects **everything**. |
| `auth_user(req)` | The authenticated user id inside a handler, or `nil`. |
| `auth_token_id(token, secret)` | The token's nonce, for a deny-list. |
| `mw_require_key(req, key)` | Constant-time check of `Bearer <key>` against one shared key (403). Not usable with `use()`; for a list of keys on the whole app use webi's `mw_require_api_key`. |

## Rotating the secret

Change `MYAPP_AUTH_SECRET` and put the old value in `previous_secrets`. New tokens use the new
secret, old tokens keep working until they expire. Drop the old secret after one `ttl_seconds`.

## What it does not do

* **No revocation.** Tokens are stateless: a stolen token works until it expires, so keep
  `ttl_seconds` short (minutes to an hour). To cut off one token, keep your own deny-list keyed
  by `auth_token_id()`; to cut off everyone, change the secret.
* **No transport security.** Send tokens over HTTPS only. webi speaks plain HTTP, so put a TLS
  proxy in front, and use `webi_production(app)` for secure cookies and headers.
* **No users, no passwords.** Store passwords with `password_hash()` and check them with
  `password_verify()`; this package only handles what comes after.

## Upgrading from 1.x

Before 2.0.0 `auth_create_token(user_id)` returned `token_<id>_<cpu clock>` (anyone could guess
it) and `mw_auth` accepted **any** non-empty `Authorization` header. Both were insecure and
have been replaced. `auth_create_token` now needs a secret and a lifetime (calling it with one
argument is an error), and `mw_auth` needs `webi_auth_configure`/`webi_auth_secret_from_env`.
Tokens issued by 1.x are not valid and must be re-issued.
