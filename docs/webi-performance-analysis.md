# webi Performance Analysis — Why Your Portfolio Server Hangs

## The Problem

When loading the portfolio page in Chrome, the server takes **36–48 seconds** to respond with a simple HTML page. The system feels like it's hanging because the server is single-threaded and blocks completely while processing each request.

## Root Cause Analysis

### 1. The `render()` function in `packages/webi/template.kh` (PRIMARY BOTTLENECK)

```kosh
fn render(template, vars):
    let result = template
    let ks = keys(vars)
    let i = 0
    while i < len(ks):
        let k = ks[i]
        let placeholder = "{{" + k + "}}"
        let v = str(vars[k])
        result = str_replace(result, placeholder, v)
        i = i + 1
    return result
```

This function calls `str_replace()` **once per template variable**. The portfolio template has **15 variables** (`name`, `initial`, `tagline`, `bio`, `about_text`, `email`, `location`, `freelance`, `github`, `linkedin`, `contact_text`, `year`).

### 2. The `str_replace()` function in `packages/webi/util.kh` (THE CULPRIT)

```kosh
fn str_replace(s, old, new):
    if len(old) == 0:
        return s
    let result = ""
    let i = 0
    let old_len = len(old)
    let s_len = len(s)
    while i < s_len:
        if i + old_len <= s_len and substring(s, i, old_len) == old:
            result = result + new
            i = i + old_len
        else:
            result = result + substring(s, i, 1)
            i = i + 1
    return result
```

**This is implemented in pure Kosh (interpreted), not native C.** Here's why it's catastrophically slow:

- **Character-by-character loop**: It iterates through every single character of the string
- **`substring()` calls**: Each iteration calls `substring()` which is itself an interpreted Kosh function
- **String concatenation (`+`)**: Each `result = result + ...` creates a **brand new string** by copying the entire accumulated result. This is O(n²) behavior.
- **Called 15 times per request**: Each call scans the entire ~24KB template

**Cost breakdown for one request:**
- Template size: ~24,000 characters
- `str_replace()` called 15 times
- Each call scans ~24,000 characters
- Each scan does character-by-character `substring()` + string concatenation
- Total: ~360,000 character operations, each involving multiple interpreted function calls

### 3. The `webi_handle()` function in `packages/webi/server.kh`

The header parsing also uses interpreted Kosh functions:
- `split()` — interpreted
- `trim()` — interpreted
- `contains()` — interpreted
- `substring()` — interpreted

While this adds some overhead, it's negligible compared to the template rendering.

### 4. Single-threaded server

The C server loop (`fn_http_serve` in `src/webi_lib.c`) is single-threaded. While one request is being processed (blocked in the interpreted `render()` function), no other connections can be accepted. This is why the entire system "hangs" — Chrome may send multiple requests (favicon, etc.) and they all queue up.

## The Fix: Implement `str_replace()` as a Native C Function

The most impactful fix is to move `str_replace()` from interpreted Kosh code to a native C function in the standard library. This would make it **100–1000x faster**.

### Implementation Plan

#### Step 1: Add `str_replace` to `src/khan_stdlib.c`

Add a native C implementation:

```c
static void fn_str_replace(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!check_arg_count(interp, "str_replace", 3, argc)) { *result = value_nil(); return; }
    const char *s, *old, *new_str;
    if (!expect_string(interp, "str_replace", 0, args[0], &s)) { *result = value_nil(); return; }
    if (!expect_string(interp, "str_replace", 1, args[1], &old)) { *result = value_nil(); return; }
    if (!expect_string(interp, "str_replace", 2, args[2], &new_str)) { *result = value_nil(); return; }

    size_t old_len = strlen(old);
    if (old_len == 0) {
        *result = value_string(s);
        return;
    }

    // First pass: count occurrences
    int count = 0;
    const char *p = s;
    while ((p = strstr(p, old)) != NULL) {
        count++;
        p += old_len;
    }

    // Calculate result size
    size_t new_len = strlen(new_str);
    size_t s_len = strlen(s);
    size_t result_len = s_len + count * (new_len - old_len);
    char *buf = malloc(result_len + 1);

    // Build result
    char *dst = buf;
    const char *src = s;
    while ((p = strstr(src, old)) != NULL) {
        size_t seg_len = p - src;
        memcpy(dst, src, seg_len);
        dst += seg_len;
        memcpy(dst, new_str, new_len);
        dst += new_len;
        src = p + old_len;
    }
    strcpy(dst, src);

    *result = value_string(buf);
    free(buf);
}
```

#### Step 2: Register it in `stdlib_register_all()`

```c
env_define(env, "str_replace", value_native("str_replace", fn_str_replace));
```

#### Step 3: Update `packages/webi/util.kh`

Remove the Kosh implementation of `str_replace()` since it will now use the native one.

#### Step 4: (Optional) Add a native `render()` function

For even better performance, you could also implement `render()` in C, which would avoid the interpreted loop over variables and the repeated `str_replace()` calls. But the native `str_replace()` alone should bring response times down from ~40 seconds to under 100ms.

## Expected Improvement

| Metric | Before (Interpreted) | After (Native C) | Improvement |
|--------|---------------------|-------------------|-------------|
| `str_replace()` per call | ~2-3 seconds | ~0.1ms | 20,000x |
| Template render (15 vars) | ~30-45 seconds | ~2ms | 15,000x |
| Total request time | ~36-48 seconds | ~5-10ms | 4,000x |

## Additional Optimizations (if needed)

1. **Cache rendered templates**: If the template and variables don't change between requests, cache the rendered HTML in memory.

2. **Implement `render()` in C natively**: This would avoid the interpreted loop overhead entirely.

3. **Use a more efficient template engine**: Instead of sequential `str_replace()` calls, do a single-pass replacement using regex or a more sophisticated parser.

4. **Add multi-threading**: For a production server, you'd want a thread pool to handle concurrent requests. But this is a much larger change.

## Summary

The root cause is that `str_replace()` — called 15 times per request on a 24KB template — is implemented in **interpreted Kosh code** rather than **native C**. Moving it to C will eliminate the 36-48 second response times and make the portfolio load instantly.