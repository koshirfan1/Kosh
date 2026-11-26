"""Generates the constant tables pasted into src/kh_ed25519.h (field constants, SHA-512 K and
IV, the group order L), so no constant in that file was typed by hand.

    python3 tools/gen_ed25519_consts.py > consts.inc

The results are checked by tests/kh/test_ed25519.py (RFC 8032 vectors, hashlib, cryptography).
"""
# Generates the constant tables for src/kh_ed25519.h (so no constant is typed by hand).
P = 2**255 - 19
def limbs(x):
    x %= P
    return [(x >> (16*i)) & 0xffff for i in range(16)]
def inv(x): return pow(x, P-2, P)
d  = (-121665 * inv(121666)) % P
d2 = (2*d) % P
By = (4 * inv(5)) % P
# x of the base point: recover from y, pick the even root
def recover_x(y, sign):
    x2 = ((y*y - 1) * inv(d*y*y + 1)) % P
    x = pow(x2, (P+3)//8, P)
    if (x*x - x2) % P: x = (x * pow(2, (P-1)//4, P)) % P
    assert (x*x - x2) % P == 0
    if x & 1 != sign: x = P - x
    return x
Bx = recover_x(By, 0)
I = pow(2, (P-1)//4, P)
def fmt(name, v): return "static const gf %s = {%s};" % (name, ",".join("0x%04x" % l for l in limbs(v)))
def isqrt_n(n, k):
    lo, hi = 0, 1 << (n.bit_length()//k + 2)
    while lo < hi:
        mid = (lo+hi+1)//2
        if mid**k <= n: lo = mid
        else: hi = mid-1
    return lo
primes = []
c = 2
while len(primes) < 80:
    if all(c % p for p in primes): primes.append(c)
    c += 1
K  = [isqrt_n(p << 192, 3) & (2**64-1) for p in primes]
H0 = [isqrt_n(p << 128, 2) & (2**64-1) for p in primes[:8]]
L = 2**252 + 27742317777372353535851937790883648493
out = []
out.append(fmt("D", d)); out.append(fmt("D2", d2)); out.append(fmt("GX", Bx)); out.append(fmt("GY", By)); out.append(fmt("SQRTM1", I))
out.append("static const uint64_t SHA512_K[80] = {\n" + ",\n".join("  0x%016xULL" % k for k in K) + "\n};")
out.append("static const uint64_t SHA512_H0[8] = {\n" + ",\n".join("  0x%016xULL" % h for h in H0) + "\n};")
out.append("static const uint8_t ED_L[32] = {" + ",".join("0x%02x" % ((L >> (8*i)) & 255) for i in range(32)) + "};")
print("\n".join(out))
