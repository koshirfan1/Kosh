# Kosh Strings Package — v1.0.0

**Package**: `strings`  
**Installation**: `kh install strings`

## Overview

The `strings` package provides comprehensive string manipulation utilities for Kosh. All functions are prefixed with `str_` for clarity and to avoid naming conflicts.

> **Note**: Kosh's built-in `substring(s, start, length)` uses **length** as the 3rd argument (not end index). All functions in this package account for this correctly.

## Installation

```bash
kh install strings
```

```kosh
import "strings"
```

---

## API Reference

### Inspection

```kosh
str_starts_with(s, prefix)    # Check if string starts with prefix → bool
str_ends_with(s, suffix)      # Check if string ends with suffix → bool
str_contains(s, sub)          # Check if string contains substring → bool
str_index_of(s, sub)          # Find first index of substring → number or -1
str_count(s, sub)             # Count non-overlapping occurrences → number
str_is_empty(s)               # Check if string is empty → bool
str_char_at(s, i)             # Get character at index i → string
```

#### Examples

```kosh
print str_starts_with("hello", "hel")   # true
print str_ends_with("hello", "lo")      # true
print str_contains("hello world", "world")  # true
print str_index_of("hello", "l")        # 2
print str_count("banana", "na")         # 2
print str_is_empty("")                  # true
print str_char_at("Kosh", 0)            # "K"
```

### Trimming

```kosh
str_trim(s)              # Strip leading and trailing whitespace
str_trim_left(s)         # Strip leading whitespace only
str_trim_right(s)        # Strip trailing whitespace only
```

#### Examples

```kosh
print str_trim("  hello  ")           # "hello"
print str_trim_left("  hello  ")      # "hello  "
print str_trim_right("  hello  ")     # "  hello"
```

### Padding & Repetition

```kosh
str_repeat(s, n)         # Repeat string n times
str_pad_left(s, width, ch)   # Left-pad to width with character ch
str_pad_right(s, width, ch)  # Right-pad to width with character ch
```

#### Examples

```kosh
print str_repeat("ab", 3)           # "ababab"
print str_pad_left("5", 3, "0")     # "005"
print str_pad_right("5", 3, "0")    # "500"
```

### Transformation

```kosh
str_replace(s, old, new_str)    # Replace all occurrences of old with new
str_reverse(s)                  # Reverse the string
str_capitalize(s)               # Capitalize first letter, lowercase rest
```

#### Examples

```kosh
print str_replace("foo bar foo", "foo", "baz")  # "baz bar baz"
print str_reverse("Kosh")                        # "nahK"
print str_capitalize("hello")                    # "Hello"
```

### Splitting & Joining

```kosh
str_split(s, delim)     # Split string by delimiter → array
str_join(arr, sep)      # Join array elements with separator → string
```

#### Examples

```kosh
print str_split("a,b,c", ",")            # ["a", "b", "c"]
print str_join(["x", "y", "z"], " | ")   # "x | y | z"
```

---

## Complete Example

```kosh
import "strings"

let text = "  Hello, Kosh World!  "

print str_trim(text)                    # "Hello, Kosh World!"
print str_starts_with(text, "  Hello")  # true
print str_contains(text, "Kosh")        # true
print str_replace(text, "World", "Universe")  # "  Hello, Kosh Universe!  "
print str_reverse("Kosh")               # "nahK"

let parts = str_split("a,b,c", ",")
print str_join(parts, " - ")            # "a - b - c"
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
