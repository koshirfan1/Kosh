# Webi Threat Model v0.1

Threat model for the Webi encrypted-channel layer (`packages/webi/secure.kh`).

## 1. Purpose

The Webi encryption layer is **not** a replacement for HTTPS. It runs **on top
of TLS** as defense in depth. This document states which attacker it protects
against, what it does not, and which claims are therefore allowed.

## 2. Assets

- Request/response bodies (HTML, JSON, forms, tokens).
- Session keys.
- Server availability (session store, handshake CPU, any DNS lookups).

## 3. Attackers

In scope:

1. Passive network attacker (public Wi-Fi, ISP).
2. Active network attacker (modifies, drops, replays packets).
3. TLS-terminating middlebox (CDN, reverse proxy, WAF, logging) that sees TLS plaintext.
4. Replay attacker re-sending a captured valid request.
5. Fake search-engine bot (spoofed User-Agent / IP) - planned, see M3.
6. Resource-exhaustion attacker (handshake flood, session-store growth).

Out of scope:

- Compromised server or developer machine.
- Browser XSS or malicious extensions: the client holds the session key, so
  anything that can run JS in the page can read decrypted data.
- Endpoint malware.
- Traffic analysis (sizes/timing) until padding exists.

## 4. What the layer adds on top of TLS

| Property | TLS alone | TLS + Webi layer |
|---|---|---|
| Network confidentiality / integrity | Yes | Yes (again) |
| Hiding bodies from a TLS-terminating proxy/CDN | No | Yes, if the proxy never holds the session key |
| Request-level replay protection | Only at TLS-record level | Yes (counter + sliding window) |
| Hiding HTML from View-Source / DevTools | No | **No** - the browser decrypts it |
| Stopping scrapers | No | Raises effort slightly; not security |

**Server authentication decision (v1):** the handshake uses ephemeral X25519
and is *not authenticated by itself*; an active attacker who can sit in the
middle of the handshake could substitute keys. v1 therefore requires the
channel to run **inside TLS**, which provides server authentication. Own
server authentication (pinned key + Ed25519 signature) is future work.

## 5. Threats and mitigations

| # | Threat | Mitigation | Status |
|---|---|---|---|
| T1 | Ciphertext tampering | ChaCha20-Poly1305; any failure -> packet dropped | Implemented, tested |
| T2 | Replay / duplicate / reorder | Per-direction counter nonce, 64-entry sliding window | Implemented, tested |
| T3 | Handshake MITM | Runs inside TLS; salt binds both public keys | Implemented (TLS required) |
| T4 | Nonce reuse | Sender counter only increases; separate key and direction id per direction; refuse after 2^48 | Implemented, tested |
| T5 | Session-store DoS | Counter instead of a nonce database (O(1) per session), session cap, TTL sweep | Implemented, tested |
| T6 | Fake search-engine bot | Forward-confirmed rDNS (host must be a subdomain of the engine's domain on a label boundary, and resolve back to the IP); a verified crawler only gets `bot_paths`, never private routes; plain responses are `private, no-store` | Implemented, tested (mocked DNS; mutation-checked) |
| T7 | DNS flood via bot checks | Lookups only for crawler claims on bot paths; positive and negative caching; per-IP (3/h) and global (20/min) budgets; over budget = treated as ordinary client. Lookups still block the single-threaded server for their duration | Implemented, tested |
| T8 | Plain-mode secret leak | `require_secure` refuses plain requests; only paths in the explicit `public` list are served in the clear (unknown paths also get 426, so route existence is not revealed) | Implemented, tested |
| T9 | Downgrade | Version field bound into the AEAD aad; unknown versions rejected | Implemented, tested |
| T10 | Key exposure to page JS | Strict CSP, key in memory only, short lifetime | Planned (M4) |

## 6. Nonce / replay design

The 12-byte nonce is a 4-byte direction id plus an 8-byte big-endian counter.
The sender's counter only increases. The receiver stores the highest counter
seen and a 64-entry window of seen counters, and only records a counter
**after** the tag verifies, so forged packets cannot move the window. This
needs O(1) memory per session; there is no table of nonces for an attacker to
fill.

## 7. Claims

Allowed (while the tests pass): "encrypted channel using ChaCha20-Poly1305 and
X25519", "replayed requests are rejected", "a TLS-terminating proxy cannot read
the body".

Not allowed: "100% secure", "unhackable", "hides HTML from View-Source", "no TLS
needed", calling a badge "proof". Proof is the live, verifiable output of a
`/webi/proof` page.

The primitives follow RFC 8439, 7748 and 5869 and pass official / OpenSSL
cross-check vectors, but the protocol built from them has had **no external
review**.

## 8. Milestones

- **M1 handshake + key schedule** - done (`secure.kh`, `tests/test_webi_secure.kh`).
- **M2 sealed packets + replay window + HTTP integration** - done (`secure_http.kh`, `tests/test_webi_secure_http.kh`, `examples/webi_secure_demo.kh`).
- **M3 bot verification** (forward-confirmed rDNS, cache, budgets, `bot_paths`) - done (`bot.kh`, `tests/test_webi_bot.kh`, `tests/test_dns.kh`).
- **M4 WASM client** (`webi.js`, < 20 KB) - pending.
- **M5 `/webi/proof` page** - pending.
- **M6 fuzzing + external crypto review before 1.0** - pending.

## 9. Open questions

1. ~~Public pages~~ decided: explicit `public` path list in `webi_enable_secure`. Verified-bot plain mode (M3) is separate.
2. Fallback for curl / old browsers: error or plain mode? (Verified crawlers are already handled by `bot_paths`.)
3. Session lifetime and key-rotation interval.
