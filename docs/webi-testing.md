# Testing and fuzzing the Webi channel

What is covered, how to run it, and how much to trust it.

## Suites

| Suite | Run with | What it checks |
|---|---|---|
| `tests/test_crypto.kh` | `./kosh tests/test_crypto.kh` | SHA-256, HMAC (RFC 4231), PBKDF2 (cross-checked with Python hashlib), `constant_time_equals` |
| `tests/test_aead.kh` | `./kosh tests/test_aead.kh` | ChaCha20-Poly1305, X25519 (RFC 7748), HKDF (RFC 5869): RFC vectors plus cases generated with OpenSSL |
| `tests/test_webi_secure.kh` | | Handshake, key schedule, sealing, replay window, tamper, store limits |
| `tests/test_webi_secure_http.kh` | | The channel inside webi: middleware on inner requests, uniform 403, rate limits, `require_secure`, `public` |
| `tests/test_webi_bot.kh`, `tests/test_dns.kh` | | Crawler verification (mock DNS) and the DNS natives |
| `tests/test_rate_limit.kh` | | The native rate limiter (regression for the old fail-open bug) |
| `tests/test_webi_client_serve.kh` | | `/_webi/client.js`, bootstrap shell, proof page, counters, generated-file drift |
| `tests/js/test_webi_js_vectors.js` | `node tests/js/test_webi_js_vectors.js` | JS client crypto: official RFC vectors and Node OpenSSL cross-checks |
| `tests/js/test_webi_js_e2e.js` | `node tests/js/test_webi_js_e2e.js` | JS client against a real Kosh server over sockets |
| `tests/js/test_webi_js_browser.js` | `JSDOM_PATH=... node tests/js/test_webi_js_browser.js` | Shell bootstrap and the proof page in jsdom, against a healthy server and a deliberately broken one |

## Fuzzing

`tests/fuzz/fuzz_webi.kh [seed] [iterations]` is a deterministic property fuzzer
(the same seed reproduces a run). Failures print the property, the seed and the
offending input and exit 1. Run it under AddressSanitizer/UBSan (CI does) so a
memory error in the native code also fails.

| Property | Meaning |
|---|---|
| P1 | random and structured garbage packets never authenticate (and always get the bare 403) |
| P2 | every single-character replacement, insertion, deletion and truncation of a valid packet (about 27,000 per run) is rejected, and forged attempts never advance the replay window |
| P3 | random handshake hellos fail cleanly or give a well-formed reply; the session cap holds |
| P4 | random AUTHENTICATED inner requests (hostile methods, paths including encoded and nested `/_webi/` tricks, headers, bodies) always get a sealed, readable reply with a sane status, and can never reach the channel's own endpoints |
| P5 | random unauthenticated requests to every `/_webi/` endpoint never crash and never produce a 5xx |
| P6 | crawler verification never accepts an address that is not genuine |
| P7 | the native AEAD never accepts random ciphertext and always round-trips |

`tests/js/fuzz_webi_js.js [seed] [scale]` fuzzes the JavaScript client:
a differential comparison against Node's OpenSSL on random inputs (AEAD of
random sizes, X25519 on arbitrary scalars and points including non-canonical
ones, HKDF, SHA-256, HMAC), exhaustive packet mutation against `Session.open`,
and random unicode bodies (every code point range, control characters, astral
characters) round-tripped through the JS client and the real Kosh server.

### How the fuzzers themselves were checked

A fuzzer that never fails proves nothing, so each was run against deliberately
planted bugs:

- Kosh fuzzer: no nesting guard, replay window updated before the tag is
  verified, session cap ignored, crawler accepted without forward confirmation,
  native AEAD tag check disabled. All five were caught.
- JS fuzzer: X25519 ignoring the high bit of the peer value (the 209 unit
  vectors did **not** catch this one; the fuzzer did), and a tag check skipped
  for longer ciphertexts. Both caught.
- The proof page in jsdom reports FAIL on a server with replay protection
  disabled.

## What is NOT covered

- **No independent cryptographic or protocol review.** Fuzzing finds
  implementation bugs, not design flaws, and passing every test here is not a
  security proof. Get an external review before relying on this layer for
  anything that matters.
- Real browsers (Chrome, Firefox, Safari) and Windows have not been exercised.
- Timing side channels: the JS client is not constant-time.
- Binary bodies (the inner message is JSON text), very large uploads, and
  multi-process / multi-machine deployments (counters and sessions live in
  process memory).
