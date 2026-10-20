# Kosh Validation Package — v1.0.0

**Package**: `validation`  
**Installation**: `kh install validation`

## Overview

The `validation` package provides basic data validation utilities for Kosh, including email format checking, string length validation, and numeric type checking.

## Installation

```bash
kh install validation
```

```kosh
import "validation"
```

---

## Functions

```kosh
is_email(s)             # Check if string is a valid email format → bool
is_min_length(s, n)     # Check if string/array length >= n → bool
is_max_length(s, n)     # Check if string/array length <= n → bool
is_numeric(s)           # Check if value is a number or numeric string → bool
trim(s)                 # Strip leading/trailing whitespace
```

#### Examples

```kosh
print is_email("user@example.com")      # true
print is_email("invalid-email")         # false
print is_min_length("hello", 3)         # true
print is_min_length("hi", 3)            # false
print is_max_length("hello", 10)        # true
print is_numeric(42)                    # true
print is_numeric("3.14")                # true
print is_numeric("abc")                 # false
print trim("  hello  ")                 # "hello"
```

---

## Complete Example

```kosh
import "validation"

let email = "user@example.com"
if is_email(email):
    print "Valid email"
else:
    print "Invalid email"

let password = "pass"
if is_min_length(password, 8):
    print "Password meets length requirement"
else:
    print "Password too short (min 8 chars)"
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
