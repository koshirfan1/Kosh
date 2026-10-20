# benchmarks/map_scale.py — same, for comparison. Python's dict is a
# real hash table, so this should be ~O(n) here.
m = {}
i = 0
while i < 4000:
    m["key" + str(i)] = i
    i = i + 1
print(len(m.keys()))
