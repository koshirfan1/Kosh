#ifndef KHAN_CRYPTO_LIB_H
#define KHAN_CRYPTO_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * crypto_lib — real SHA-256, HMAC-SHA256, and PBKDF2-HMAC-SHA256, closing
 * the gap flagged in examples/task_tracker/README.md ("no password
 * hashing native in Khan yet"). Self-contained (public-domain-style
 * SHA-256 written directly in C, no external crypto library) rather than
 * bridging to OpenSSL - no new build-time dependency for something this
 * small and this standard, unlike llama.cpp or libsqlite3.
 *
 * Every function here is tested against known SHA-256/HMAC test vectors
 * before being trusted (see chat) - shipping unverified hand-written
 * crypto would be worse than not having it.
 *
 * Registers:
 *   sha256(str)                       -> hex string (64 chars)
 *   password_hash(password)           -> string, self-contained: encodes
 *       a random 16-byte salt + 100,000 PBKDF2-HMAC-SHA256 iterations +
 *       the derived key, as "pbkdf2_sha256$100000$<salt_hex>$<hash_hex>".
 *       Salted and iterated - NOT the same as calling sha256() on a
 *       password directly, which would be unsuitable for password
 *       storage (fast hashes are brute-forceable; this deliberately
 *       is not fast).
 *   password_verify(password, stored) -> bool - re-derives with the
 *       salt/iteration count embedded in `stored` and compares.
 *
 * Scope, stated plainly: this is PBKDF2-HMAC-SHA256, not bcrypt/argon2.
 * A reasonable, genuinely-salted-and-iterated choice for a language that
 * currently has zero hashing primitives - not a claim that it's the
 * strongest option available in 2026.
 */
void crypto_register_all(Environment *env);
void crypto_register_all_vm(VM *vm);

#endif
