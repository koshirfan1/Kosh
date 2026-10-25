#ifndef KOSH_CRYPTO_AEAD_H
#define KOSH_CRYPTO_AEAD_H

#include "interpreter.h"
#include "vm.h"

/*
 * crypto_aead - ChaCha20-Poly1305 (RFC 8439) and X25519 (RFC 7748), written
 * directly in C, no external crypto library. Binary values cross the Kosh
 * boundary as lowercase hex strings (Kosh strings are NUL-terminated).
 *
 *   chacha20poly1305_encrypt(key_hex, nonce_hex, aad, plaintext) -> hex
 *       key = 32 bytes (64 hex), nonce = 12 bytes (24 hex). Returns
 *       ciphertext || 16-byte tag as hex. NEVER reuse a (key, nonce) pair.
 *   chacha20poly1305_decrypt(key_hex, nonce_hex, aad, ct_hex) -> string | nil
 *       Returns nil if the tag does not verify (tampered / wrong key / wrong
 *       aad / wrong nonce). Plaintext must not contain NUL bytes.
 *   x25519_public(private_hex)               -> public key hex (32 bytes)
 *   x25519_shared(private_hex, peer_pub_hex) -> shared secret hex, or nil if
 *       the peer key is a low-order point (all-zero result).
 *
 * Generate private keys with secure_token(32). Run the raw x25519_shared
 * output through hkdf_sha256() before using it as an AEAD key.
 */
void crypto_aead_register_all_vm(VM *vm);

#endif
