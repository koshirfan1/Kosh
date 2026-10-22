# Kosh Requests Package — v1.1.0

**Package**: `requests`  
**Installation**: `kh install requests`

## Overview

The `requests` package provides a comprehensive HTTP client for Kosh, built on top of the native `http_get`, `http_post`, `http_post_json`, `http_put`, and `http_delete` built-in functions. Version 1.1 adds custom headers, sessions, retry logic, query string builders, and response helpers.

Every function returns a response map with the following structure:

| Field | Type | Description |
|-------|------|-------------|
| `status` | number | HTTP status code (e.g. 200) |
| `body` | string | Raw response body |
| `success` | bool | `true` if status is 200–299 |
| `json` | map/array/nil | Decoded JSON body (nil if not JSON) |

## Installation

```bash
kh install requests
```

```kosh
import "requests"
```

---

## Response Helpers

```kosh
ok(res)              # Check if request succeeded (status 2xx) → bool
status(res)          # Get HTTP status code → number
body(res)            # Get raw response body → string
text(res)            # Alias for body() → string
json(res)            # Get decoded JSON body → map/array/nil
is_json(res)         # Check if response body is JSON → bool
safe_json(res, fallback)  # Decode JSON or return fallback
raise_for_status(res)     # Print error if request failed, return res
```

#### Examples

```kosh
let res = get("https://api.example.com/users")
if ok(res):
    print "Status: " + str(status(res))
    let data = json(res)
    print data[0]["name"]
else:
    raise_for_status(res)
```

---

## HTTP Methods

### GET

```kosh
get(url)              # GET request with auto JSON decode
get_h(url, headers)   # GET with custom headers
get_retry(url, n)     # GET with retry (n attempts on failure)
```

#### Examples

```kosh
let res = get("https://jsonplaceholder.typicode.com/todos/1")
print json(res)["title"]

# With custom headers
let headers = "Authorization: Bearer token123"
let res2 = get_h("https://api.example.com/protected", headers)

# With retry
let res3 = get_retry("https://api.example.com/unstable", 3)
```

### POST

```kosh
post(url, body)              # POST with form-encoded body
post_h(url, body, headers)   # POST with custom headers
post_json(url, data)         # POST with JSON body (auto-encoded)
post_json_h(url, data, headers)  # POST JSON with custom headers
post_retry(url, body, n)     # POST with retry
```

#### Examples

```kosh
# Form-encoded POST
let res = post("https://api.example.com/login", "user=irfan&pass=secret")

# JSON POST
let data = {"name": "Irfan", "lang": "Kosh"}
let res2 = post_json("https://api.example.com/users", data)

# JSON POST with auth header
let res3 = post_json_h("https://api.example.com/users", data,
                       "Authorization: Bearer token123")
```

### PUT

```kosh
put(url, body)              # PUT request
put_h(url, body, headers)   # PUT with custom headers
put_json(url, data)         # PUT with JSON body
```

#### Example

```kosh
let res = put_json("https://api.example.com/users/1",
                   {"name": "Irfan Updated"})
```

### PATCH

```kosh
patch(url, body)            # PATCH request (partial update)
patch_json(url, data)       # PATCH with JSON body
```

#### Example

```kosh
let res = patch_json("https://api.example.com/users/1",
                     {"lang": "Kosh v2"})
```

### DELETE

```kosh
delete(url)                 # DELETE request
delete_h(url, headers)      # DELETE with custom headers
```

#### Example

```kosh
let res = delete("https://api.example.com/users/1")
```

### HEAD

```kosh
head(url)                   # HEAD request (returns headers only, body empty)
```

#### Example

```kosh
let res = head("https://api.example.com")
print status(res)           # 200
```

---

## URL & Query Builders

```kosh
build_query(params)         # Build query string from map
build_url(base, params)     # Append query string to URL
encode_form(params)         # Encode map as form-encoded string
```

#### Examples

```kosh
let params = {"page": "2", "limit": "10"}
print build_query(params)           # "page=2&limit=10"

let url = build_url("https://api.example.com/users", params)
print url                           # "https://api.example.com/users?page=2&limit=10"

let form = encode_form({"user": "irfan", "age": "25"})
print form                          # "user=irfan&age=25"
```

---

## Sessions

Sessions provide a shared base URL and default headers context, eliminating repetitive URL and header specification.

```kosh
session(base_url)                   # Create a new session
session_set_header(s, key, value)   # Add default header to session
session_get(s, path)                # GET relative to base URL
session_post(s, path, body)         # POST relative to base URL
session_post_json(s, path, data)    # POST JSON relative to base URL
session_put(s, path, body)          # PUT relative to base URL
session_delete(s, path)             # DELETE relative to base URL
```

#### Example

```kosh
let s = session("https://jsonplaceholder.typicode.com")
s = session_set_header(s, "Authorization", "Bearer mytoken")

let res1 = session_get(s, "/todos/1")
let res2 = session_post_json(s, "/todos", {"title": "Learn Kosh", "completed": false})
let res3 = session_delete(s, "/todos/1")
```

---

## Complete Example

```kosh
import "requests"

# Simple GET
let res = get("https://jsonplaceholder.typicode.com/todos/1")
if ok(res):
    let todo = json(res)
    print "Todo: " + todo["title"]
    print "Completed: " + str(todo["completed"])

# POST JSON
let newTodo = {"title": "Learn Kosh", "completed": false}
let res2 = post_json("https://jsonplaceholder.typicode.com/todos", newTodo)
print "Created: " + str(status(res2))

# Session-based workflow
let s = session("https://jsonplaceholder.typicode.com")
let users = session_get(s, "/users")
let firstUser = json(users)[0]
print "User: " + firstUser["name"]
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.1.0 | Added headers support, sessions, retry, query builder, PATCH, HEAD, response helpers |
| 1.0.0 | Initial release: get, post, post_json, put, delete |
