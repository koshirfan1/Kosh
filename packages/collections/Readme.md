# Khan Collections Package — v1.0.0

**Package**: `collections`  
**Installation**: `kh install collections`

## Overview

The `collections` package provides functional array helpers for Khan. Since Khan has no built-in sort, filter, map, or reduce, this package fills that gap with higher-order functions that take a named function as an argument.

> **Important**: In Khan, a function's closure snapshots the enclosing scope at the moment it is **declared**, not when it is called. Helper functions must be declared **before** any function that calls them — there is no forward reference or hoisting.

## Installation

```bash
kh install collections
```

```khan
import "collections"
```

---

## Basic Operations

```khan
reverse_arr(arr)        # Return reversed copy of array
arr_contains(arr, val)  # Check if array contains value → bool
```

#### Examples

```khan
print reverse_arr([1, 2, 3])        # [3, 2, 1]
print arr_contains([1, 2, 3], 2)    # true
print arr_contains([1, 2, 3], 5)    # false
```

---

## Higher-Order Functions

These functions take a **named function** as an argument. Define your predicate or transform function first, then pass its name.

### Filter

```khan
filter(arr, pred)       # Return array of elements where pred(x) is true
```

#### Example

```khan
fn is_even(n):
    return n % 2 == 0

print filter([1, 2, 3, 4, 5, 6], is_even)   # [2, 4, 6]
```

### Map

```khan
map_each(arr, transform_fn)  # Transform each element
```

#### Example

```khan
fn double(n):
    return n * 2

print map_each([1, 2, 3], double)   # [2, 4, 6]
```

### Reduce

```khan
reduce(arr, reducer, initial)  # Accumulate values left-to-right
```

#### Example

```khan
fn sum(acc, x):
    return acc + x

print reduce([1, 2, 3, 4], sum, 0)   # 10
```

### Find

```khan
find(arr, pred)         # Return first element matching pred, or nil
find_index(arr, pred)   # Return index of first match, or -1
```

#### Example

```khan
fn is_positive(n):
    return n > 0

print find([-3, -2, 0, 1, 2], is_positive)   # 1
print find_index([-3, -2, 0, 1, 2], is_positive)  # 3
```

### Matching

```khan
any_match(arr, pred)        # True if any element matches
all_match(arr, pred)        # True if all elements match
count_matching(arr, pred)   # Count matching elements
```

#### Examples

```khan
fn is_odd(n):
    return n % 2 == 1

print any_match([2, 4, 6], is_odd)      # false
print any_match([2, 3, 4], is_odd)      # true
print all_match([2, 4, 6], is_odd)      # false
print count_matching([1, 2, 3, 4, 5], is_odd)  # 3
```

---

## Sorting

### Sort Numbers

```khan
sort(arr)               # Ascending sort (numbers only, insertion sort)
sort_desc(arr)          # Descending sort
```

#### Example

```khan
print sort([3, 1, 4, 1, 5, 9])         # [1, 1, 3, 4, 5, 9]
print sort_desc([3, 1, 4, 1, 5, 9])    # [9, 5, 4, 3, 1, 1]
```

### Sort by Key

```khan
sort_by(arr, key_fn)        # Sort by numeric key extracted from each element
sort_by_desc(arr, key_fn)   # Sort descending by key
```

#### Example

```khan
fn get_age(person):
    return person["age"]

let people = [
    {"name": "Alice", "age": 30},
    {"name": "Bob", "age": 25},
    {"name": "Charlie", "age": 35}
]

let sorted = sort_by(people, get_age)
# [{name: "Bob", age: 25}, {name: "Alice", age: 30}, {name: "Charlie", age: 35}]
```

---

## Utility Operations

```khan
chunk(arr, size)        # Split array into chunks of given size
zip(a, b)               # Pair up elements from two arrays
unique(arr)             # Remove duplicates (preserves first occurrence)
```

#### Examples

```khan
print chunk([1, 2, 3, 4, 5], 2)    # [[1, 2], [3, 4], [5]]
print zip([1, 2, 3], ["a", "b", "c"])  # [[1, "a"], [2, "b"], [3, "c"]]
print unique([1, 2, 2, 3, 1, 4])   # [1, 2, 3, 4]
```

---

## Complete Example

```khan
import "collections"

# Define helper functions
fn is_even(n):
    return n % 2 == 0

fn square(n):
    return n * n

fn sum(acc, x):
    return acc + x

fn get_score(player):
    return player["score"]

# Data
let numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
let players = [
    {"name": "Alice", "score": 95},
    {"name": "Bob", "score": 87},
    {"name": "Charlie", "score": 92}
]

# Pipeline
let evens = filter(numbers, is_even)        # [2, 4, 6, 8, 10]
let squares = map_each(evens, square)       # [4, 16, 36, 64, 100]
let total = reduce(squares, sum, 0)         # 220

print "Sum of squares of evens: " + str(total)

# Sort players by score
let ranked = sort_by(players, get_score)
for p in ranked:
    print p["name"] + ": " + str(p["score"])
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |