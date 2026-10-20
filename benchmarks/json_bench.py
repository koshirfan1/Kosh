# benchmarks/json_bench.py — same encode+decode round-trip, 50,000 iterations.
import json
obj = {"name": "khan", "version": 1, "tags": ["a", "b", "c", "d", "e"], "nested": {"x": 1, "y": 2, "z": 3}}
i = 0
while i < 50000:
    s = json.dumps(obj)
    d = json.loads(s)
    i = i + 1
print("done")
