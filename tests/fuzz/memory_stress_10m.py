#!/usr/bin/env python3
"""
tests/fuzz/memory_stress_10m.py — the roadmap's own explicit ask for
item #7 (Memory manager): "10 million allocations. Leak? Fragmentation?"

Runs a real Khan script that performs 10 million map allocations (one
per loop iteration, immediately becoming unreachable garbage the next
iteration — exactly the shape of workload that would reveal an
OP_RETURN/OP_DEF_GLOBAL-style leak if one still existed), while
sampling the process's actual resident memory (RSS) every 0.5s via
psutil. A leak shows up as RSS climbing roughly linearly with the
number of completed iterations and never plateauing. No leak shows up
as RSS climbing briefly (allocator growing its pool / OS page cache
warming up) then plateauing well before the halfway point.

This is a live-process RSS check, not valgrind — it can run all 10
million iterations in the time valgrind would need for a few hundred
thousand, at the cost of being a coarser signal (OS-level resident
memory, not byte-exact leak accounting). Pair with valgrind on a
smaller iteration count (docs/memory-notes.md) for byte-exact
confirmation; use this for "does it actually hold up at the scale the
roadmap asked for."

Usage: python3 tests/fuzz/memory_stress_10m.py [khan_binary]
"""
import subprocess
import sys
import time
import os
import tempfile

try:
    import psutil
except ImportError:
    print("Requires psutil (pip install psutil). Skipping — this check "
          "is supplementary to the byte-exact valgrind audit in "
          "docs/memory-notes.md, not a replacement for it.")
    sys.exit(0)

KHAN_BIN = sys.argv[1] if len(sys.argv) > 1 else "./khan"
ITERATIONS = 10_000_000
SAMPLE_INTERVAL = 0.5

# Two scenarios: uniform-size allocations (the roadmap's literal "10
# million allocations" ask) and variable-size allocations (what
# actually stresses fragmentation — uniform-size allocations reuse the
# same-sized freed slot every time, which can't reveal fragmentation
# from a mix of allocation sizes competing for the same heap space).
SCENARIOS = {
    "uniform": f"""
let i = 0
let sum = 0
while i < {ITERATIONS}:
    let m = {{"n": i}}
    sum = sum + m["n"]
    i = i + 1
print sum
""",
    "variable": f"""
let i = 0
let total_len = 0
while i < {ITERATIONS // 5}:
    let size = i % 97
    let arr = range(0, size)
    total_len = total_len + len(arr)
    i = i + 1
print total_len
""",
    "closures": f"""
fn make_adder(x):
    fn adder(y):
        return x + y
    return adder

let i = 0
let total = 0
while i < {ITERATIONS}:
    let f = make_adder(i)
    total = total + f(1)
    i = i + 1
print total
""",
}


def run_scenario(name, script, khan_bin):
    script_path = os.path.join(tempfile.gettempdir(), f"_memstress_{name}.kh")
    with open(script_path, "w") as f:
        f.write(script)

    print(f"\n{'=' * 60}\nScenario: {name}\n{'=' * 60}")
    t0 = time.time()
    proc = subprocess.Popen([khan_bin, script_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ps_proc = psutil.Process(proc.pid)

    samples = []
    while proc.poll() is None:
        try:
            rss_mb = ps_proc.memory_info().rss / (1024 * 1024)
            samples.append((time.time() - t0, rss_mb))
        except psutil.NoSuchProcess:
            break
        time.sleep(SAMPLE_INTERVAL)

    stdout, stderr = proc.communicate()
    elapsed = time.time() - t0
    os.remove(script_path)

    if proc.returncode != 0:
        print(f"FAILED: khan exited with code {proc.returncode}")
        print(stderr.decode(errors="replace")[:500])
        return False

    print(f"Completed in {elapsed:.1f}s. Output: {stdout.decode().strip()}")

    if len(samples) < 4:
        print("Too few RSS samples (ran too fast) to judge a trend — inconclusive, not a failure.")
        return True

    n = len(samples)
    idxs = sorted(set([0, 1, 2, n // 2 - 1, n // 2, n // 2 + 1, n - 3, n - 2, n - 1]) & set(range(n)))
    for i in idxs:
        t, m = samples[i]
        print(f"  t={t:6.1f}s  RSS={m:8.1f} MB")

    half = n // 2
    first_half_growth = samples[half][1] - samples[0][1]
    second_half_growth = samples[-1][1] - samples[half][1]
    print(f"First-half growth: {first_half_growth:+.1f} MB   Second-half growth: {second_half_growth:+.1f} MB")

    if second_half_growth > first_half_growth * 0.5 and second_half_growth > 20:
        print("SIGNAL OF A LEAK/UNBOUNDED GROWTH in this scenario.")
        return False

    print("No leak/fragmentation-growth signal — plateaus well before completion.")
    return True


def main():
    print(f"Sampling RSS every {SAMPLE_INTERVAL}s across three scenarios "
          f"({ITERATIONS:,} uniform-size allocs, {ITERATIONS // 5:,} variable-size allocs, "
          f"{ITERATIONS:,} closure creations).")

    ok = True
    for name, script in SCENARIOS.items():
        ok = run_scenario(name, script, KHAN_BIN) and ok

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
