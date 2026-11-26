/*
 * kh_pubkey.h - the public key kh uses to check packages/registry.json.
 *
 * KH_REGISTRY_PUBKEY_HEX is the Ed25519 PUBLIC key (64 hex characters) whose
 * private half signs the registry (see docs/SIGNING.md and tools/sign_registry.py).
 *
 *   ""  (empty)  -> UNSIGNED MODE: kh cannot verify the registry's origin, only the
 *                   SHA-256 checksums inside it, and says so on every run.
 *   64 hex chars -> kh REFUSES any registry without a valid signature
 *                   (packages/registry.json.sig) made by the matching private key.
 *
 * A public key is not a secret, so it is committed. The PRIVATE key must never be
 * committed or stored in the repository.
 */
#ifndef KH_PUBKEY_H
#define KH_PUBKEY_H
#define KH_REGISTRY_PUBKEY_HEX ""
#endif
