# benchmarks/loop.py — same tight loop, 5,000,000 iterations.
total = 0
i = 0
while i < 5000000:
    total = total + i
    i = i + 1
print(total)
