# benchmarks/string_concat.py — same repeated concatenation, 20,000 iterations.
s = ""
i = 0
while i < 20000:
    s = s + "x"
    i = i + 1
print(len(s))
