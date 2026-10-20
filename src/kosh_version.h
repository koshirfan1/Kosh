#ifndef KOSH_VERSION_H
#define KOSH_VERSION_H

/* Single source of truth for Kosh's version string. Bump this by hand
 * when it actually matters (a real release, a breaking change worth
 * flagging) — nothing in the build auto-increments it.
 *
 * "0.x" deliberately, not "1.0" — ROADMAP_STATUS_UPDATED.md's own v1.0
 * release criteria aren't met yet (see that file), and claiming 1.0 in
 * `kosh --version` while the roadmap says otherwise would be exactly
 * the kind of inconsistency this project's docs otherwise go out of
 * their way to avoid. Bump to 1.0.0 when the roadmap says so, not
 * before. */
#define KOSH_VERSION "0.015.0"

#endif
