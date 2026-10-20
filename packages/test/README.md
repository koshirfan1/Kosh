# Kosh Test Package — v1.0.0

**Author**: Irfan Khan  
**Package**: `test`  
**Installation**: `kh install test`

## Overview

The `test` package provides a lightweight unit testing framework for Kosh. It supports test suites with assertions, pass/fail/skip tracking, and formatted reporting.

## Installation

```bash
kh install test
```

```kosh
import "test"
```

---

## Creating a Test Suite

```kosh
test_suite(name)        # Create a new test suite
```

#### Example

```kosh
let t = test_suite("My Math Tests")
```

---

## Assertions

```kosh
test_true(t, label, value)          # Assert value is true
test_false(t, label, value)         # Assert value is false
test_eq(t, label, actual, expected) # Assert equality
test_neq(t, label, actual, unexpected)  # Assert inequality
test_nil(t, label, value)           # Assert value is nil
test_not_nil(t, label, value)       # Assert value is not nil
test_gt(t, label, actual, than)     # Assert actual > than
test_lt(t, label, actual, than)     # Assert actual < than
test_gte(t, label, actual, than)    # Assert actual >= than
test_lte(t, label, actual, than)    # Assert actual <= than
test_contains_str(t, label, haystack, needle)  # Assert string contains substring
test_len(t, label, arr, expected_len)  # Assert array length
test_type(t, label, value, expected_type)  # Assert value type
test_skip(t, label)                 # Mark test as skipped
```

#### Examples

```kosh
t = test_true(t, "true is true", true)
t = test_false(t, "false is false", false)
t = test_eq(t, "1+1 equals 2", 1 + 1, 2)
t = test_neq(t, "1+1 not 3", 1 + 1, 3)
t = test_nil(t, "nil is nil", nil)
t = test_gt(t, "5 > 3", 5, 3)
t = test_lt(t, "3 < 5", 3, 5)
t = test_contains_str(t, "hello contains ell", "hello", "ell")
t = test_len(t, "array length", [1, 2, 3], 3)
t = test_type(t, "type is number", 42, "number")
t = test_skip(t, "not implemented yet")
```

---

## Running Tests

```kosh
test_run(t)             # Run suite and print formatted report → bool (true if all passed)
test_summary(t)         # Get summary map without printing
```

#### Example Output

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  TEST SUITE: My Math Tests
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  ✓  PASS  true is true
  ✓  PASS  1+1 equals 2
  ✗  FAIL  5 < 3
           → 5 is not < 3
  ⏭  SKIP  not implemented yet
────────────────────────────────────────
  2/4 passed   |   1 failed   |   1 skipped
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## Complete Example

```kosh
import "test"

# Create suite
let t = test_suite("Calculator Tests")

# Test a function
fn add(a, b):
    return a + b

fn subtract(a, b):
    return a - b

# Run assertions
t = test_eq(t, "add(2, 3) = 5", add(2, 3), 5)
t = test_eq(t, "subtract(10, 4) = 6", subtract(10, 4), 6)
t = test_neq(t, "add(1, 1) != 3", add(1, 1), 3)
t = test_true(t, "add result is number", type(add(1, 1)) == "number")
t = test_skip(t, "multiplication test - TODO")

# Run and get result
let all_passed = test_run(t)
if all_passed:
    print "All tests passed!"
else:
    print "Some tests failed."
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
