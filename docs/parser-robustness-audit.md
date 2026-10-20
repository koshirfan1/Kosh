# Parser Robustness Audit

Roadmap item #2 asked for the parser to never crash on invalid syntax,
and for systematic negative-case/fuzz testing to confirm it. This is
that testing.

## What exists

`tests/fuzz/negative_cases.py` — 38 hand-picked genuinely-invalid
programs (incomplete expressions, unclosed delimiters, malformed
literals, invalid characters, broken control flow, malformed
declarations, malformed imports, indentation errors) that must all fail
to compile (exit 65) and must fail *cleanly*, not crash. Plus 9
"robustness" cases — syntactically valid-but-extreme input (empty
files, 500-deep nested parens/brackets, a 5000-term expression) that
must not crash or hang, without a strict exit-code requirement since
some legitimately succeed.

`tests/fuzz/random_fuzz.py` — mutation-based fuzzing. Takes the
project's own real `.kh` files (104 of them, across `examples/`,
`tests/suites/`, and every package) as a seed corpus, applies 1-5 random
byte-level mutations (flip/delete/insert/truncate/duplicate) per
iteration, and runs the mutant through the interpreter with a timeout.
Run three times during this audit at increasing scale (500, 2000, and
3000 iterations, different random seeds) — **5,500 total mutations,
zero genuine crashes or hangs found.**

## Two false-positive categories worth knowing about

Both showed up while building this suite, and both are documented
rather than "fixed" because they aren't bugs — they're correct
behavior that a naive "did it exit within N seconds" check can't tell
apart from a real hang without a little more care:

1. **Seed files that intentionally start a real server**
   (`webi_run()`/`http_serve()` — `examples/webi_diag.kh`,
   `examples/webi_app.kh`, `examples/webi_security_server_demo.kh`,
   and the `webi` package's own `server.kh`/`webi.kh`). Any mutation
   that doesn't happen to break the syntax before reaching that call
   will hang forever waiting for connections — correctly, by design.
   `random_fuzz.py` excludes any seed file containing those call
   markers from its corpus rather than fuzzing them.

2. **A mutation that creates a legitimate infinite loop in the mutated
   program's own logic.** Found one concrete example: a mutation
   dedented a while-loop's increment statement (`i = i + 1`) out of
   the loop body, leaving `while i < 3: print(i)` with no way to ever
   reach 3 — which correctly runs forever printing `0`, the same as
   `while True: print(0)` would in any language. This is not an
   interpreter bug. Distinguished from a genuinely *stuck* process
   (deadlocked, spinning with no progress) by checking whether the
   killed process had already produced substantial output right up to
   the timeout — active, ongoing output means "this mutated program is
   busy correctly looping forever," not "the interpreter got stuck."

## Documented parser leniencies (found, not fixed)

Three inputs I initially expected to be errors turned out to be
accepted — investigated each individually rather than assuming they
were bugs:

- **Top-level `return`** ends the script early. This is intentional —
  `vm_run()` explicitly special-cases `frame_count` dropping below its
  starting value to support exactly this.
- **`fn foo:` with no parentheses at all** parses as a valid
  zero-argument function. Works correctly, doesn't crash — whether this
  *should* be required syntax is a language-design call for the
  maintainer, not a robustness bug.
- **Dedenting to an indentation level that was never pushed onto the
  indent stack** (e.g. 2 spaces, when only 0 and 4 were ever seen) is
  silently accepted rather than raising an IndentationError the way
  Python does. Doesn't crash, just more permissive than it could be.
  Flagged for whoever picks up the roadmap's separate "better
  indentation handling" item — not fixed here, since it's scoped as
  its own future lexer item, not a robustness bug.

## Running this yourself

```sh
python3 tests/fuzz/negative_cases.py ./kosh
python3 tests/fuzz/random_fuzz.py ./kosh 1000 <any-seed-number>
```

Both are wired into CI (`.github/workflows/c-cpp.yml`) on every push —
1000 mutation iterations per run, cheap enough (~1.5s total for both
scripts combined) to run on every CI build rather than being a
special/manual-only check.

## Honest scope note

This is not a claim that the parser is bug-free — 5,500 mutations plus
38 curated cases is a real but bounded sample, not exhaustive coverage.
A dedicated, longer-running fuzz campaign (the kind AFL/libFuzzer-style
tools are built for, run for hours/days rather than seconds) would
likely find more, especially in less-traveled corners of the grammar.
What this establishes is a baseline: the parser survived everything
thrown at it here, and there's now a fast, repeatable, CI-integrated
way to keep checking that as the grammar evolves, rather than no
systematic check existing at all (which was the state before this).
