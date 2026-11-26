/* Test driver for src/kh_ed25519.h. Reads lines from stdin:
 *   <pubkey hex> <sig hex> <message hex or "-" for empty>
 * prints "1" or "0" per line (valid / invalid), plus a SHA-512 mode:
 *   sha512 <message hex or "-">  -> prints the digest hex
 * Driven by tests/kh/test_ed25519.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/kh_ed25519.h"

static int unhex(const char *s, uint8_t *out, size_t max) {
    if (strcmp(s, "-") == 0) return 0;
    size_t n = strlen(s) / 2;
    if (n > max) return -1;
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(s + 2*i, "%2x", &v); out[i] = (uint8_t)v; }
    return (int)n;
}

int main(void) {
    static char l1[600000], l2[600000], l3[600000];
    static uint8_t msg[300000];
    char cmd[32];
    while (scanf("%31s", cmd) == 1) {
        if (strcmp(cmd, "sha512") == 0) {
            if (scanf("%599999s", l1) != 1) return 2;
            int n = unhex(l1, msg, sizeof msg);
            sha512_ctx c; uint8_t d[64];
            sha512_init(&c); sha512_update(&c, msg, (size_t)n); sha512_final(&c, d);
            for (int i = 0; i < 64; i++) printf("%02x", d[i]);
            printf("\n");
        } else if (strcmp(cmd, "verify") == 0) {
            if (scanf("%599999s %599999s %599999s", l1, l2, l3) != 3) return 2;
            uint8_t pk[32], sig[64];
            if (unhex(l1, pk, 32) != 32 || unhex(l2, sig, 64) != 64) { printf("0\n"); continue; }
            int n = unhex(l3, msg, sizeof msg);
            printf("%d\n", ed25519_verify(sig, msg, (size_t)n, pk));
        }
    }
    return 0;
}
