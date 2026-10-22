#!/usr/bin/env python3
"""
benchmarks/run.py — runs each benchmark under every available language
runtime, times wall-clock execution, and prints a comparison table.

Usage: python3 benchmarks/run.py
(Run from the repository root, or pass --kosh-path to point at a
non-default kosh binary.)

This does NOT try to be a rigorous microbenchmark harness (no warmup
iterations, no statistical repeats/variance reporting) — it's meant to
give an honest order-of-magnitude picture, not publishable numbers.
Each benchmark runs once; if you need tighter numbers, wrap this in a
loop and average.
"""
import subprocess
import sys
import time
import shutil
import os

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))

BENCHMARKS = ["loop", "fib", "string_concat", "json_bench", "map_scale"]

RUNTIMES = {
    "kosh":   {"ext": "kh", "cmd": lambda path: [KOSH_BIN, path]},
    "python": {"ext": "py", "cmd": lambda path: [sys.executable, path]},
    "node":   {"ext": "js", "cmd": lambda path: ["node", path]},
    "c":      {"ext": "c",  "cmd": None},  # compiled separately, see below
}


def find_kosh_binary():
    for candidate in ("./kosh", "./kosh.exe", "kosh", "kosh.exe"):
        p = os.path.join(os.path.dirname(BENCH_DIR), candidate.lstrip("./"))
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    found = shutil.which("kosh")
    if found:
        return found
    return None


KOSH_BIN = find_kosh_binary()


def compile_c(name):
    src = os.path.join(BENCH_DIR, f"{name}.c")
    if not os.path.isfile(src):
        return None
    out = os.path.join(BENCH_DIR, f"_{name}_c_bin")
    result = subprocess.run(["gcc", "-O2", src, "-o", out], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  (C compile failed for {name}: {result.stderr.strip()[:200]})")
        return None
    return out


def time_run(cmd):
    t0 = time.time()
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=BENCH_DIR)
    elapsed = time.time() - t0
    if result.returncode != 0:
        return None, result.stderr.strip()[:200]
    return elapsed, result.stdout.strip()


def main():
    rows = []
    for bench in BENCHMARKS:
        row = {"benchmark": bench}

        # kosh
        kosh_src = os.path.join(BENCH_DIR, f"{bench}.kh")
        if KOSH_BIN and os.path.isfile(kosh_src):
            elapsed, out = time_run([KOSH_BIN, kosh_src])
            row["kosh"] = elapsed
        else:
            row["kosh"] = None

        # python
        py_src = os.path.join(BENCH_DIR, f"{bench}.py")
        if os.path.isfile(py_src):
            elapsed, out = time_run([sys.executable, py_src])
            row["python"] = elapsed
        else:
            row["python"] = None

        # node
        js_src = os.path.join(BENCH_DIR, f"{bench}.js")
        if os.path.isfile(js_src) and shutil.which("node"):
            elapsed, out = time_run(["node", js_src])
            row["node"] = elapsed
        else:
            row["node"] = None

        # c
        c_bin = compile_c(bench)
        if c_bin:
            elapsed, out = time_run([c_bin])
            row["c"] = elapsed
        else:
            row["c"] = None

        rows.append(row)

    # print table
    langs = ["kosh", "python", "node", "c"]
    header = f"{'benchmark':<16}" + "".join(f"{l:>12}" for l in langs) + "     kosh/fastest"
    print(header)
    print("-" * len(header))
    for row in rows:
        times = {l: row[l] for l in langs if row[l] is not None}
        fastest = min(times.values()) if times else None
        ratio = f"{row['kosh']/fastest:>10.1f}x" if row["kosh"] and fastest else "      n/a"
        cells = "".join(
            f"{(f'{row[l]:.3f}s' if row[l] is not None else 'n/a'):>12}" for l in langs
        )
        print(f"{row['benchmark']:<16}{cells}{ratio}")

    # cleanup compiled C binaries
    for bench in BENCHMARKS:
        p = os.path.join(BENCH_DIR, f"_{bench}_c_bin")
        if os.path.isfile(p):
            os.remove(p)


if __name__ == "__main__":
    main()
