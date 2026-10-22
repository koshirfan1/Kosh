# Classes (v1)

Kosh has Python-style classes: a class is a set of methods, calling the
class builds an instance, `self` is an ordinary explicit first
parameter, and single inheritance (`class Dog(Animal):`) with `super`
is supported.

```
class Dog:
    fn __init__(self, name, age):
        self.name = name
        self.age = age

    fn birthday(self):
        self.age = self.age + 1
        return self          # returning self allows chaining

let d = Dog("Rex", 3)
d.birthday().birthday()
print d.age                  # 5

class Puppy(Dog):
    fn __init__(self, name, age):
        super.__init__(name, age)   # runs Dog.__init__
    fn birthday(self):
        return super.birthday() + 1 # Dog's birthday, plus one more

let p = Puppy("Fido", 1)
print p.birthday()           # 3
```

## What works

| Feature | Notes |
|---|---|
| `class Name:` with `fn` methods | Only `fn` declarations are allowed directly inside a class body (parse error otherwise). |
| `Name(args)` | Builds an instance and runs `__init__` if defined. With no `__init__`, the class takes zero arguments. |
| `__init__` return value | Discarded. The call expression always evaluates to the instance. |
| `obj.field` / `obj.field = v` | Fields are dynamic: assigning creates them. Reading a missing field gives `nil` (same as a missing map key). |
| `obj.method(args)` | The receiver is passed as `self`. Argument count includes `self` when checked, so error messages say "methods take 'self' first". |
| Chaining | `a.inc().inc()` works; so do method calls in the middle of expressions. |
| `type(x)` | `"class"` for a class, `"instance"` for an instance. |
| `print` / `str()` | `<class Dog>` and `<Dog instance>`. |
| `==` | Identity. Two separately built instances are never equal. |
| Reference semantics | `let b = a` aliases the same instance, like arrays and maps. |
| `try`/`catch` | A `throw` or runtime error inside a method or `__init__` unwinds correctly, including inside `super.method()` and across multiple inheritance levels. |
| `class Dog(Animal):` | Single inheritance. `Animal` must already be a defined class by the time this statement runs (ordinary top-down script execution, same requirement Python has). |
| Method/constructor lookup | Checks the instance's own class first, then walks up the superclass chain — an override always wins; an inherited method/`__init__` is found automatically if the subclass doesn't define its own. |
| `super.method(args)` | Only valid inside a method of a class declared with a `(Base)` clause. Resolved **statically** against that class's declared superclass (and *its* chain), not the receiver's dynamic class — the usual meaning of `super`. |

## What does not (yet)

- **No multiple inheritance.** `class C(A, B):` is not supported — only one base class.
- **No bound-method values.** `let m = obj.method` is not supported;
  methods are only reachable through call syntax `obj.method(...)`.
  This is why `obj.method(args)` is its own AST node
  (`AST_METHOD_CALL`) rather than `AST_GET_ATTR` + a call.
- **No class-level fields or static methods.** A class holds methods only.
- **No `isinstance`-style checks.**
- **Cycles through instance fields leak.** Instances are reference
  counted but not registered with the cycle collector (same documented
  gap as closures). `a.friend = b; b.friend = a` leaks both; acyclic
  structures are freed normally.
- **Function registry limit.** Every method is a registered function, and
  the registry holds 1024 (`KOSHFN_REGISTRY_MAX` in `src/vm.c`), shared
  with ordinary functions.
- **Classes declared inside a function** work (the class is rebuilt and
  rebound as a global each time the function runs, like a nested `fn`),
  but each call creates a new, distinct class value.

## How it works

**Values.** `VAL_CLASS` and `VAL_INSTANCE` are new value types. A class's
`Obj` also carries `super_ref`, a retained pointer to its immediate
superclass's `Obj` (NULL with no `(Base)` clause). Both are
heap `Obj`s whose `as.map` storage is laid out exactly like a `VAL_MAP`
(a class's map is `method name -> function`, an instance's is
`field name -> value`), so `map_get`/`map_set` and the hash index are
reused as-is. An instance also holds a retained `class_ref` pointer to
its class, which is how a method call finds its method. Instances live
outside the cycle collector; see above.

**Compiling a class.** `AST_CLASS_STMT` compiles each method like a
top-level `fn` (`compile_class_method`, no global definition at the end),
pushes `[class name][superclass or nil][method name][fn index]...`, then
emits `OP_MAKE_CLASS` (or `_WIDE` for more than 255 methods) and binds the
result to a global named after the class. The superclass slot is either
`OP_NIL` (no `(Base)` clause) or `emit_global_get(Base)` — which already
raises a runtime error if `Base` isn't defined, so `OP_MAKE_CLASS` only
needs to check that what it popped is nil or actually a `VAL_CLASS`.
Because of that, `Dog(...)` is compiled as an ordinary call; nothing
special happens at the call site.

**Method/constructor lookup** (`class_method_lookup` in `vm.c`) checks a
class's own method map first, then walks `super_ref` up the chain,
returning the first match — so an override always wins, and an inherited
method is found automatically. `OP_CALL`'s instantiation branch and
`OP_CALL_METHOD` both use this for `__init__` and ordinary method
dispatch respectively.

**`super.method(args)`.** Compiling a method inside `class Dog(Animal):`
records `Animal`'s name on that method's `CompilerState`
(`class_super_name`), not resolved to a value until the call actually
runs. `super.method(args)` then compiles to: push `self`
(`OP_GET_LOCAL 0` — self is always local slot 0 in a method), push the
arguments, then `OP_CALL_SUPER` with the superclass name and method name
baked in as constant operands (always 2-byte indices; super calls are
rare enough that a narrow variant isn't worth a second opcode). At
runtime, `OP_CALL_SUPER` looks up the named superclass as a global,
resolves the method by walking *that* class's chain (`class_method_lookup`
again — so `super.foo()` finds `foo` even if the immediate parent doesn't
define it but a grandparent does), and runs it inline with the same
`is_method_call` convention as `OP_CALL_METHOD` (`self` already on the
stack, no hidden callee slot below it).

A real bug, caught by valgrind rather than by the test suite: the global
class value `OP_CALL_SUPER` looks up is a **shallow, borrowed** copy —
`table_get`/`global_get` just alias the stored `Obj*` without bumping its
refcount, the same convention `OP_GET_GLOBAL` relies on when it explicitly
`value_copy()`s the result before pushing it. The first version of
`OP_CALL_SUPER` called `value_free()` on that borrowed copy after setting
up the call frame, dropping a reference it never owned and freeing the
live superclass `Obj` out from under the rest of the program. Nothing
broke immediately — the corruption was invisible until some *later*,
unrelated method call walked the now-freed class and crashed inside
`class_method_lookup`. Fixed by simply never freeing it;
`tests/suites/classes.kh` has a regression test that calls a `super`
method and then an unrelated inherited method afterward, specifically to
catch this class of bug again.

**Opcodes.**

| Opcode | Stack effect |
|---|---|
| `OP_MAKE_CLASS[_WIDE]` | `[name][mname][fn_idx]... -> [class]` |
| `OP_CALL_METHOD[_WIDE]` | `[receiver][args...] ->` runs the method inline; receiver is `self` (slot 0) |
| `OP_GET_ATTR[_WIDE]` | `[obj] -> [value]` |
| `OP_SET_ATTR[_WIDE]` | `[obj][value] -> [value]` |
| `OP_CALL` on a `VAL_CLASS` | overwrites the class's stack slot with the new instance, then runs `__init__` inline (or just leaves the instance) |

**Two call-frame flags** (in `CallFrame`) exist because these calls have a
different stack layout from a plain function call, where a hidden callee
slot sits just below the arguments:

- `is_method_call`: there is no callee slot below `self`, so `OP_RETURN`
  truncates the stack to `slots`, not `slots - 1`. Getting this wrong
  corrupts the caller's expression temporaries.
- `is_constructor`: on `OP_RETURN`, yield `self` (`slots[0]`) instead of
  `__init__`'s own return value.

**Why `__init__` runs inline.** The first implementation ran `__init__`
through `vm_call_value`, which starts a nested `run_loop`. A `throw` inside
`__init__` unwound across that boundary and left the stack unbalanced
("stack underflow" on the next statement). Running it as a normal frame in
the caller's dispatch loop fixes that class of bug, and
`tests/suites/classes.kh` has a regression test for it.

## Tests

`tests/suites/classes.kh`, registered in `tests/run_all.kh`:

- **`suite_classes`** (25 checks) — construction, fields, chaining,
  temporaries around method calls, a 5,000-iteration stack-balance loop,
  reference semantics, instances in arrays and nested instances, `type()`,
  every error path through `try`/`catch`, and the constructor-throw
  regression.
- **`suite_inheritance`** (15 checks) — overriding vs. inheriting,
  `super.__init__`/`super.method()`, a subclass with no `__init__` of its
  own, two-level inheritance (`Puppy(Dog)` where `Dog(Animal)`), a
  2,000-iteration mixed super-call/inherited-call loop, undefined
  superclass names, and the borrowed-reference regression above.
