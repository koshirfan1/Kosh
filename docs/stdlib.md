# Built-in libraries and standard library

[← Back to the README](../README.md)

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

### Hashing, crypto and DNS builtins

Native functions (no package needed). Binary values (keys, nonces, ciphertexts)
cross the language boundary as lowercase **hex strings**, because Kosh strings are
NUL-terminated. The AEAD and X25519 functions follow RFC 8439 and RFC 7748, and
`hkdf_sha256` follows RFC 5869; none of this has had an independent review.

| Function | Returns | Notes |
|---|---|---|
| `sha256(text)` | hex string | SHA-256 of the string's bytes |
| `hmac_sha256(key, message)` | hex string | HMAC-SHA256 |
| `hkdf_sha256(ikm_hex, salt_hex, info, length)` | hex string | RFC 5869; `salt_hex` may be `""`; `length` 1-255 bytes |
| `constant_time_equals(a, b)` | bool | Use this, not `==`, to compare secrets such as API keys and MACs |
| `password_hash(password)` | string | PBKDF2-HMAC-SHA256 with a random per-hash salt from the OS CSPRNG |
| `password_verify(password, stored)` | bool | Constant-time compare; rejects malformed hashes and out-of-range iteration counts |
| `secure_token(n)` | hex string | `n` random bytes from the OS CSPRNG |
| `x25519_public(private_hex)` | hex string | 32-byte public key |
| `x25519_shared(private_hex, peer_public_hex)` | hex string or `nil` | `nil` for a low-order peer key. Run the result through `hkdf_sha256` before using it as a key |
| `chacha20poly1305_encrypt(key_hex, nonce_hex, aad, plaintext)` | hex string | Ciphertext followed by the 16-byte tag. **Never reuse a (key, nonce) pair** |
| `chacha20poly1305_decrypt(key_hex, nonce_hex, aad, ct_hex)` | string or `nil` | `nil` if the tag does not verify. Plaintext must not contain NUL bytes |
| `rate_limit_check(key, max_requests, window_seconds)` | bool | Per-key counter in process memory (4096 keys; resets on restart; not shared between processes) |
| `dns_reverse(ip)` | hostname or `nil` | Blocking reverse lookup |
| `dns_resolves_to(hostname, ip)` | bool | Blocking forward lookup; true if `ip` is among the results |

`substring(s, start, length)` takes a **length**, not an end index.
