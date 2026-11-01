# The Kosh language

[← Back to the README](../README.md)

## Language Features

### Core Language

- ✅ **Indentation-based scoping** — Spaces define block structure (like Python)
- ✅ **Variable declarations** — `let` keyword
- ✅ **Function definitions** — `fn` keyword with parameters and closures
- ✅ **Recursion** — Functions can call themselves
- ✅ **Conditionals** — `if` / `elif` / `else`
- ✅ **While loops** — `while condition:`
- ✅ **For-in loops** — `for var in expr:`
- ✅ **Break & Continue** — `break` and `continue` inside loops
- ✅ **Return statements** — `return expr` from functions
- ✅ **Closures** — Functions capture their enclosing lexical scope
- ✅ **Import/Module system** — `import "filename.kh"` or `import "packagename"`, plus [`from "pkg" import A, B, C`](from-import.md) for pulling out individual symbols, the package's own name, or a sibling submodule file
- ✅ **String escape sequences** — `\n`, `\t`, `\r`, `\\`, `\"`, `\0`, `\xHH`

### Data Types

- ✅ **Numbers** — Integers and floating-point (double precision)
- ✅ **Strings** — Double-quoted with full escape sequence support
- ✅ **Booleans** — `true` / `false`
- ✅ **Nil** — `nil`
- ✅ **Arrays** — `[1, 2, 3]` with zero-based indexing
- ✅ **Maps** — `{"key": value}` dictionaries

### Operators

- ✅ **Arithmetic** — `+`, `-`, `*`, `/`, `%`
- ✅ **Comparison** — `==`, `!=`, `<`, `<=`, `>`, `>=`
- ✅ **Logical** — `and`, `or`, `not`
- ✅ **String concatenation** — `+`
- ✅ **Indexing** — `arr[i]`, `map["key"]`

---

## Syntax & Examples

### Variables & Types

```kosh
let name    = "Irfan"
let age     = 25
let active  = true
let nothing = nil
let scores  = [95, 87, 92]
let person  = {"name": "Irfan", "city": "Faisalabad"}
```

### Conditionals with elif

```kosh
fn grade(score):
    if score >= 90:
        return "A"
    elif score >= 80:
        return "B"
    elif score >= 70:
        return "C"
    else:
        return "F"

print grade(85)   # B
```

### Loops with break and continue

```kosh
# break
let i = 0
while true:
    if i >= 5:
        break
    i = i + 1
print i   # 5

# continue — skip evens
for n in [1, 2, 3, 4, 5, 6]:
    if n % 2 == 0:
        continue
    print n   # 1 3 5
```

### Recursion

```kosh
fn factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

print factorial(10)   # 3628800

fn fib(n):
    if n <= 1:
        return n
    return fib(n - 1) + fib(n - 2)

print fib(10)   # 55
```

### String Escape Sequences

```kosh
print "Hello\nWorld"         # newline
print "Tab:\there"           # tab
print "Quote: \"Hi\""        # double quote
print "\x1b[31mRed\x1b[0m"  # ANSI color (hex escape)
```
