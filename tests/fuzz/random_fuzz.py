#!/usr/bin/env python3
"""
tests/fuzz/random_fuzz.py — mutation-based fuzzing using real Khan
source files (from examples/, tests/, packages/) as seed corpus.

Unlike negative_cases.py's hand-picked cases, this explores input space
a human wouldn't necessarily think to write by hand: takes real,
valid .kh files and randomly corrupts them (byte flips, deletions,
insertions, truncation) many times, running each mutant through the
interpreter with a timeout. The only thing checked is "did it crash or
hang" — a mutant is expected to usually fail to parse, and that's fine;
a clean SyntaxError or even a clean runtime error is a pass. Only a
segfault/abort/hang counts as a finding.

Usage:
  python3 tests/fuzz/random_fuzz.py [khan_binary] [iterations] [seed]

Defaults: ./khan, 500 iterations, a fixed seed (for reproducibility —
pass a different seed or 0 for a randomized run).
"""
import subprocess
import sys
import os
import random
import glob
import tempfile

KHAN_BIN = sys.argv[1] if len(sys.argv) > 1 else "./khan"
ITERATIONS = int(sys.argv[2]) if len(sys.argv) > 2 else 500
SEED = int(sys.argv[3]) if len(sys.argv) > 3 else 42
TIMEOUT_SECS = 3

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Files that intentionally start a real, blocking server (webi_run(), etc.)
# — any mutation that doesn't happen to break the syntax before reaching
# that call will hang *by design*, not because of a fuzzer-found bug.
# Confirmed by running this fuzzer once without this exclusion: all 4
# findings from a 500-iteration run traced back to exactly one seed file
# (webi_security_server_demo.kh) whose own header comment says outright
# "a script that starts webi_run() blocks forever". Excluding known
# blocking scripts rather than lowering the timeout, since a short
# timeout would risk false-positive "hangs" on legitimately slow-but-
# finite scripts (e.g. examples/vision_test_all.kh's real cascade scan).
BLOCKING_CALL_MARKERS = ("webi_run(", "http_serve(")


def is_blocking_script(path):
    try:
        with open(path, "r", errors="ignore") as f:
            content = f.read()
    except OSError:
        return False
    return any(marker in content for marker in BLOCKING_CALL_MARKERS)


def find_seed_files():
    patterns = [
        "examples/*.kh",
        "tests/suites/*.kh",
        "packages/*/*.kh",
    ]
    files = []
    for pat in patterns:
        files.extend(glob.glob(os.path.join(REPO_ROOT, pat)))
    files = [f for f in files if os.path.getsize(f) > 0]
    excluded = [f for f in files if is_blocking_script(f)]
    if excluded:
        print(f"Excluding {len(excluded)} seed file(s) that start a real server (hang by design):")
        for f in excluded:
            print(f"  {f}")
    return [f for f in files if f not in excluded]


def mutate(data: bytearray, rng: random.Random) -> bytearray:
    """Apply 1-5 random mutations to a copy of the source bytes."""
    data = bytearray(data)
    n_mutations = rng.randint(1, 5)
    for _ in range(n_mutations):
        if len(data) == 0:
            break
        kind = rng.choice(["flip", "delete", "insert", "truncate", "duplicate"])
        pos = rng.randint(0, len(data) - 1)
        if kind == "flip":
            data[pos] = rng.randint(0, 255)
        elif kind == "delete":
            del data[pos]
        elif kind == "insert":
            data.insert(pos, rng.randint(0, 255))
        elif kind == "truncate":
            data = data[:pos]
        elif kind == "duplicate" and len(data) > 1:
            chunk_len = rng.randint(1, min(20, len(data) - pos))
            chunk = data[pos:pos + chunk_len]
            data[pos:pos] = chunk
    return data


def run_mutant(path):
    try:
        result = subprocess.run(
            [KHAN_BIN, path], capture_output=True, timeout=TIMEOUT_SECS
        )
        return result.returncode, None
    except subprocess.TimeoutExpired as e:
        # Python's subprocess captures whatever partial output the
        # process produced before being killed, even on timeout. A
        # mutation that (e.g.) dedents a loop's increment statement out
        # of the loop body creates a syntactically-valid infinite loop
        # in the MUTATED PROGRAM's own logic — `while i < 3: print(i)`
        # with the increment now unreachable runs forever printing "0",
        # the same as it would in any language. That's not an
        # interpreter bug, so it needs to be distinguished from a
        # genuinely stuck process (waiting on I/O, deadlocked, spinning
        # with no progress). Heuristic: substantial output actively
        # produced right up to the kill = the mutated program is
        # (correctly) busy-looping, not stuck.
        output_len = len(e.stdout) if e.stdout else 0
        if output_len > 1000:
            return "ACTIVE_LOOP", output_len
        return "HANG", output_len


def classify(code):
    if code == "ACTIVE_LOOP":
        return "ACTIVE_LOOP"
    if code == "HANG":
        return "HANG"
    if isinstance(code, int) and code < 0:
        return "CRASH"
    return "NORMAL"


def main():
    seeds = find_seed_files()
    if not seeds:
        print("No seed files found — check REPO_ROOT/glob patterns.")
        sys.exit(1)
    print(f"Found {len(seeds)} seed files. Running {ITERATIONS} mutations (seed={SEED})...")

    rng = random.Random(SEED)
    findings = []
    active_loops = 0
    mutant_path = os.path.join(tempfile.gettempdir(), "_fuzz_mutant.kh")

    for i in range(ITERATIONS):
        seed_path = rng.choice(seeds)
        with open(seed_path, "rb") as f:
            original = f.read()

        mutated = mutate(original, rng)
        with open(mutant_path, "wb") as f:
            f.write(mutated)

        code, extra = run_mutant(mutant_path)
        status = classify(code)

        if status == "ACTIVE_LOOP":
            active_loops += 1
        elif status in ("HANG", "CRASH"):
            findings.append((i, seed_path, status, code, bytes(mutated)))

        if (i + 1) % 100 == 0:
            print(f"  ...{i + 1}/{ITERATIONS} done, {len(findings)} findings, "
                  f"{active_loops} active-loop mutants (not bugs) so far")

    if os.path.exists(mutant_path):
        os.remove(mutant_path)

    print(f"\n{ITERATIONS} mutations run across {len(seeds)} seed files.")
    print(f"{active_loops} mutant(s) created a legitimate infinite loop in their own "
          f"logic (confirmed actively producing output, not stuck) — expected and not "
          f"a bug, same as `while True: print(0)` would do in any language.")
    if findings:
        print(f"\n{len(findings)} genuine CRASH/HANG finding(s) (stuck with no/little progress, or killed by a signal):")
        for i, seed_path, status, code, mutated in findings[:10]:
            save_path = os.path.join(tempfile.gettempdir(), f"_fuzz_finding_{i}.kh")
            with open(save_path, "wb") as f:
                f.write(mutated)
            print(f"  iteration {i}: seed={seed_path} status={status} exit={code} saved={save_path}")
        sys.exit(1)

    print("No crashes or stuck hangs found.")
    sys.exit(0)


if __name__ == "__main__":
    main()
