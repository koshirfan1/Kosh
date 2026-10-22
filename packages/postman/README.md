# Kosh Postman Package — v1.0.0

**Package**: `postman`  
**Installation**: `kh install postman`

## Overview

The `postman` package is an API testing toolkit for Kosh. It provides a context-based testing framework where you chain HTTP requests and assertions through a shared `pm` context map, then generate a formatted test report.

## Installation

```bash
kh install postman
```

```kosh
import "postman"
```

---

## Creating a Test Context

```kosh
pm_new()                        # Create a new empty test context
pm_new_with_base(base_url)      # Create context with a base URL for relative paths
```

#### Example

```kosh
let pm = pm_new()
# or with base URL:
let pm = pm_new_with_base("https://jsonplaceholder.typicode.com")
```

---

## HTTP Request Tests

Each function sends an HTTP request, records the result, and returns the updated context.

```kosh
pm_get(pm, name, url)           # Test a GET request
pm_post(pm, name, url, body)    # Test a POST request
pm_post_json(pm, name, url, data)  # Test a POST with JSON body
pm_put(pm, name, url, body)     # Test a PUT request
pm_delete(pm, name, url)        # Test a DELETE request
```

If a base URL was set, paths starting with `/` are resolved relative to it.

#### Examples

```kosh
let pm = pm_new_with_base("https://jsonplaceholder.typicode.com")
pm = pm_get(pm, "GET todo", "/todos/1")
pm = pm_post_json(pm, "POST todo", "/todos", {"title": "Learn Kosh", "completed": false})
```

---

## Assertions

```kosh
pm_assert(pm, name, condition)              # Assert a boolean condition
pm_expect_status(pm, name, res, expected)   # Assert HTTP status code
pm_expect_body_contains(pm, name, res, sub) # Assert body contains substring
pm_expect_json_field(pm, name, res, field, expected)  # Assert JSON field value
```

#### Examples

```kosh
# Assert a plain condition
pm = pm_assert(pm, "math works", 1 + 1 == 2)

# Assert response status
let fake = {"status": 200, "body": "{\"id\": 1}", "success": true}
pm = pm_expect_status(pm, "status 200", fake, 200)

# Assert JSON field
pm = pm_expect_json_field(pm, "id equals 1", fake, "id", 1)

# Assert body contains text
pm = pm_expect_body_contains(pm, "contains hello", fake, "id")
```

---

## Running Tests

```kosh
pm_run(pm)      # Print the full formatted test report
```

The report displays:
- PASS/FAIL status for each test
- HTTP status codes
- Failure details
- Summary with total, passed, and failed counts

#### Example Output

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

## Complete Example

```kosh
import "postman"

# Create test context with base URL
let pm = pm_new_with_base("https://jsonplaceholder.typicode.com")

# HTTP request tests
pm = pm_get(pm,      "GET todo",   "/todos/1")
pm = pm_post_json(pm, "POST todo", "/todos", {"title": "Learn Kosh", "completed": false})

# Assertions
pm = pm_assert(pm,   "math works",  1 + 1 == 2)

# Test with mock response
let fake = {"status": 200, "body": "{\"id\": 1}", "success": true}
pm = pm_expect_status(pm,     "status 200",  fake, 200)
pm = pm_expect_json_field(pm, "id equals 1", fake, "id", 1)

# Print report
pm_run(pm)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
