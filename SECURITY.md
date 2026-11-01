# Security policy

## Supported versions

Kosh is pre-1.0. Only the latest release (see [CHANGELOG.md](CHANGELOG.md)) gets fixes.

## Reporting a vulnerability

Please **do not** open a public issue with exploit details.

Use GitHub's private reporting: on the repository's **Security** tab choose
**Report a vulnerability**. (The maintainer needs to have private vulnerability
reporting enabled under Settings > Code security for that button to appear.)

If it is not available, open a public issue that says only that you have a
security report and how you would like to be contacted, with no technical details,
and the maintainer will reach out.

This is a one-person open-source project: reports are handled on a best-effort
basis and there is no guaranteed response time.

## What is in scope

- Memory-safety bugs in the interpreter, VM or native libraries (`src/`).
- Shell-command, path or injection issues in `kh`, `requests` and `webi`.
- Flaws in the Webi encrypted channel, the crawler verification, the rate limiter or
  the browser client.

## Know before you report or rely on it

- The Webi encrypted channel (`webi` 1.2.0, opt-in) **has not had an independent
  cryptographic or protocol review**, so findings there are expected and welcome.
  It is designed to run on top of TLS. See [docs/webi-threat-model.md](docs/webi-threat-model.md)
  for what it does and does not claim, and [docs/webi-testing.md](docs/webi-testing.md)
  for what has been tested.
- `kh install` does not verify checksums of downloaded packages yet.

## Out of scope

- Issues that need an attacker who already controls the machine running Kosh, or
  script running in the page for the browser client (documented limitation).
- Denial of service through resource exhaustion by a trusted local user.
