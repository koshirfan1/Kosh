#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "crypto_aead.h"

/* ---------------------------------------------------------------- helpers */

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes exactly `want` bytes (2*want hex chars). Returns 1 on success. */
static int hex_decode_exact(const char *hex, unsigned char *out, size_t want) {
    if (strlen(hex) != want * 2) return 0;
    for (size_t i = 0; i < want; i++) {
        int hi = hex_val(hex[2*i]), lo = hex_val(hex[2*i+1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = d[in[i] >> 4]; out[2*i+1] = d[in[i] & 15]; }
    out[2*n] = 0;
}

static uint32_t ld32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void st64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static int ct_eq16(const unsigned char *a, const unsigned char *b) {
    unsigned char d = 0;
    for (int i = 0; i < 16; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* --------------------------------------------------------------- ChaCha20 */

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7);

static void chacha20_block(const unsigned char key[32], uint32_t counter,
                           const unsigned char nonce[12], unsigned char out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    s[13] = ld32(nonce); s[14] = ld32(nonce + 4); s[15] = ld32(nonce + 8);
    memcpy(x, s, sizeof(s));
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
}

static void chacha20_xor(const unsigned char key[32], uint32_t counter,
                         const unsigned char nonce[12],
                         const unsigned char *in, unsigned char *out, size_t len) {
    unsigned char block[64];
    while (len > 0) {
        chacha20_block(key, counter++, nonce, block);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ block[i];
        in += n; out += n; len -= n;
    }
}

/* --------------------------------------------------------------- Poly1305 */

static void poly1305_mac(unsigned char mac[16], const unsigned char *m, size_t bytes,
                         const unsigned char key[32]) {
    const uint32_t M = 0x3ffffff;
    uint32_t r0 = ld32(key + 0) & 0x3ffffff;
    uint32_t r1 = (ld32(key + 3) >> 2) & 0x3ffff03;
    uint32_t r2 = (ld32(key + 6) >> 4) & 0x3ffc0ff;
    uint32_t r3 = (ld32(key + 9) >> 6) & 0x3f03fff;
    uint32_t r4 = (ld32(key + 12) >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    while (bytes > 0) {
        unsigned char blk[16] = {0};
        size_t n = bytes >= 16 ? 16 : bytes;
        uint32_t hibit = (n == 16) ? (1u << 24) : 0;
        memcpy(blk, m, n);
        if (n < 16) blk[n] = 1;

        h0 += ld32(blk + 0) & M;
        h1 += (ld32(blk + 3) >> 2) & M;
        h2 += (ld32(blk + 6) >> 4) & M;
        h3 += (ld32(blk + 9) >> 6) & M;
        h4 += (ld32(blk + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & M; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & M; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & M; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & M; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & M;
        h0 += c * 5; c = h0 >> 26; h0 &= M; h1 += c;

        m += n; bytes -= n;
    }

    /* full carry */
    uint32_t c;
    c = h1 >> 26; h1 &= M; h2 += c;
    c = h2 >> 26; h2 &= M; h3 += c;
    c = h3 >> 26; h3 &= M; h4 += c;
    c = h4 >> 26; h4 &= M; h0 += c * 5;
    c = h0 >> 26; h0 &= M; h1 += c;

    /* compute h + -p */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= M;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= M;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= M;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= M;
    uint32_t g4 = h4 + c - (1u << 26);

    /* select h if h < p, else h - p (constant time) */
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    /* h = h % 2^128 */
    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

    /* mac = (h + pad) % 2^128 */
    uint64_t f;
    f = (uint64_t)h0 + ld32(key + 16);            h0 = (uint32_t)f;
    f = (uint64_t)h1 + ld32(key + 20) + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + ld32(key + 24) + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + ld32(key + 28) + (f >> 32); h3 = (uint32_t)f;
    st32(mac + 0, h0); st32(mac + 4, h1); st32(mac + 8, h2); st32(mac + 12, h3);
}

/* ------------------------------------------------- AEAD (RFC 8439, 2.8) */

/* Builds the Poly1305 input: aad | pad16 | ct | pad16 | len(aad) | len(ct). */
static unsigned char *aead_mac_input(const unsigned char *aad, size_t aad_len,
                                     const unsigned char *ct, size_t ct_len, size_t *out_len) {
    size_t pa = (16 - aad_len % 16) % 16, pc = (16 - ct_len % 16) % 16;
    size_t total = aad_len + pa + ct_len + pc + 16;
    unsigned char *buf = calloc(total ? total : 1, 1);
    if (!buf) return NULL;
    size_t o = 0;
    if (aad_len) memcpy(buf + o, aad, aad_len);
    o += aad_len + pa;
    if (ct_len) memcpy(buf + o, ct, ct_len);
    o += ct_len + pc;
    st64(buf + o, (uint64_t)aad_len);
    st64(buf + o + 8, (uint64_t)ct_len);
    *out_len = total;
    return buf;
}

static int aead_tag(unsigned char tag[16], const unsigned char key[32], const unsigned char nonce[12],
                    const unsigned char *aad, size_t aad_len, const unsigned char *ct, size_t ct_len) {
    unsigned char block0[64];
    chacha20_block(key, 0, nonce, block0);          /* one-time Poly1305 key = first 32 bytes */
    size_t n;
    unsigned char *mi = aead_mac_input(aad, aad_len, ct, ct_len, &n);
    if (!mi) return 0;
    poly1305_mac(tag, mi, n, block0);
    free(mi);
    memset(block0, 0, sizeof(block0));
    return 1;
}

#define AEAD_MAX_PLAINTEXT (64u * 1024u * 1024u)

void fn_chacha20poly1305_encrypt(Value *result, Interpreter *interp, int argc, Value *args) {
    *result = value_nil();
    if (argc < 4 || args[0].type != VAL_STRING || args[1].type != VAL_STRING ||
        args[2].type != VAL_STRING || args[3].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: chacha20poly1305_encrypt(key_hex, nonce_hex, aad, plaintext) expects 4 strings\n");
        interp->had_runtime_error = 1; return;
    }
    unsigned char key[32], nonce[12];
    if (!hex_decode_exact(args[0].as.string, key, 32) || !hex_decode_exact(args[1].as.string, nonce, 12)) {
        fprintf(stderr, "Runtime error: chacha20poly1305_encrypt() key must be 64 hex chars and nonce 24 hex chars\n");
        interp->had_runtime_error = 1; return;
    }
    const unsigned char *aad = (const unsigned char*)args[2].as.string;
    size_t aad_len = strlen(args[2].as.string);
    const unsigned char *pt = (const unsigned char*)args[3].as.string;
    size_t pt_len = strlen(args[3].as.string);
    if (pt_len > AEAD_MAX_PLAINTEXT) { interp->had_runtime_error = 1; return; }

    unsigned char *ct = malloc(pt_len + 16);
    char *hex = malloc((pt_len + 16) * 2 + 1);
    if (!ct || !hex) { free(ct); free(hex); interp->had_runtime_error = 1; return; }
    chacha20_xor(key, 1, nonce, pt, ct, pt_len);
    if (!aead_tag(ct + pt_len, key, nonce, aad, aad_len, ct, pt_len)) { free(ct); free(hex); interp->had_runtime_error = 1; return; }
    hex_encode(ct, pt_len + 16, hex);
    *result = value_string(hex);
    free(ct); free(hex);
    memset(key, 0, sizeof(key));
}

void fn_chacha20poly1305_decrypt(Value *result, Interpreter *interp, int argc, Value *args) {
    *result = value_nil();
    if (argc < 4 || args[0].type != VAL_STRING || args[1].type != VAL_STRING ||
        args[2].type != VAL_STRING || args[3].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: chacha20poly1305_decrypt(key_hex, nonce_hex, aad, ct_hex) expects 4 strings\n");
        interp->had_runtime_error = 1; return;
    }
    unsigned char key[32], nonce[12];
    if (!hex_decode_exact(args[0].as.string, key, 32) || !hex_decode_exact(args[1].as.string, nonce, 12)) {
        fprintf(stderr, "Runtime error: chacha20poly1305_decrypt() key must be 64 hex chars and nonce 24 hex chars\n");
        interp->had_runtime_error = 1; return;
    }
    const char *cthex = args[3].as.string;
    size_t hl = strlen(cthex);
    if (hl < 32 || hl % 2 != 0 || hl / 2 - 16 > AEAD_MAX_PLAINTEXT) return;   /* too short for a tag -> nil */
    size_t total = hl / 2, ct_len = total - 16;
    unsigned char *buf = malloc(total);
    if (!buf) { interp->had_runtime_error = 1; return; }
    if (!hex_decode_exact(cthex, buf, total)) { free(buf); return; }

    const unsigned char *aad = (const unsigned char*)args[2].as.string;
    unsigned char tag[16];
    if (!aead_tag(tag, key, nonce, aad, strlen(args[2].as.string), buf, ct_len) ||
        !ct_eq16(tag, buf + ct_len)) {
        free(buf); memset(key, 0, sizeof(key)); return;              /* authentication failed -> nil */
    }
    char *pt = malloc(ct_len + 1);
    if (!pt) { free(buf); interp->had_runtime_error = 1; return; }
    chacha20_xor(key, 1, nonce, buf, (unsigned char*)pt, ct_len);
    pt[ct_len] = 0;
    *result = value_string(pt);
    free(pt); free(buf);
    memset(key, 0, sizeof(key));
}

/* ----------------------------------------------------------------- X25519
 * Field arithmetic over GF(2^255-19) with 16 x 16-bit limbs held in int64,
 * the TweetNaCl formulation. The Montgomery ladder uses constant-time
 * conditional swaps; no secret-dependent branches or table lookups.     */

typedef int64_t gf[16];
static const gf _121665 = {0xDB41, 1};

static void car25519(gf o) {
    int64_t c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * (1LL << 16);
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t c = ~((int64_t)b - 1), t;
    for (int i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}

static void pack25519(unsigned char *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = (unsigned char)(t[i] & 0xff); o[2*i+1] = (unsigned char)(t[i] >> 8); }
}

static void unpack25519(gf o, const unsigned char *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void finv(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) {
        fsq(c, c);
        if (a != 2 && a != 4) fmul(c, c, in);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void x25519(unsigned char q[32], const unsigned char n[32], const unsigned char p[32]) {
    unsigned char z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fsq(d, e); fsq(f, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fsq(b, a); fsub(c, d, f);
        fmul(a, c, _121665); fadd(a, a, d); fmul(c, c, a); fmul(a, d, f);
        fmul(d, b, x); fsq(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    finv((int64_t*)(x + 32), (int64_t*)(x + 32));
    fmul((int64_t*)(x + 16), (int64_t*)(x + 16), (int64_t*)(x + 32));
    pack25519(q, (int64_t*)(x + 16));
    memset(z, 0, sizeof(z));
}

void fn_x25519_public(Value *result, Interpreter *interp, int argc, Value *args) {
    *result = value_nil();
    unsigned char priv[32], pub[32], base[32] = {9};
    if (argc < 1 || args[0].type != VAL_STRING || !hex_decode_exact(args[0].as.string, priv, 32)) {
        fprintf(stderr, "Runtime error: x25519_public(private_hex) expects 64 hex chars\n");
        interp->had_runtime_error = 1; return;
    }
    x25519(pub, priv, base);
    char hex[65]; hex_encode(pub, 32, hex);
    *result = value_string(hex);
    memset(priv, 0, sizeof(priv));
}

void fn_x25519_shared(Value *result, Interpreter *interp, int argc, Value *args) {
    *result = value_nil();
    unsigned char priv[32], peer[32], out[32];
    if (argc < 2 || args[0].type != VAL_STRING || args[1].type != VAL_STRING ||
        !hex_decode_exact(args[0].as.string, priv, 32) || !hex_decode_exact(args[1].as.string, peer, 32)) {
        fprintf(stderr, "Runtime error: x25519_shared(private_hex, peer_public_hex) expects two 64-char hex strings\n");
        interp->had_runtime_error = 1; return;
    }
    x25519(out, priv, peer);
    unsigned char acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    if (acc != 0) {                                  /* all-zero => low-order peer point: refuse */
        char hex[65]; hex_encode(out, 32, hex);
        *result = value_string(hex);
    }
    memset(priv, 0, sizeof(priv)); memset(out, 0, sizeof(out));
}

void crypto_aead_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "chacha20poly1305_encrypt", fn_chacha20poly1305_encrypt);
    vm_global_set_native(vm, "chacha20poly1305_decrypt", fn_chacha20poly1305_decrypt);
    vm_global_set_native(vm, "x25519_public",            fn_x25519_public);
    vm_global_set_native(vm, "x25519_shared",            fn_x25519_shared);
}
