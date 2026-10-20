#!/usr/bin/env bash
# Kosh installer — mirrors the "curl | bash" experience of rustup,
# deno, bun, etc. There's no pre-built binary release yet (see
# ROADMAP_STATUS_UPDATED.md — v1.0 hasn't shipped), so this builds
# from source rather than downloading a prebuilt binary. That's the
# honest state of things: this script does real work (checks for a
# compiler, clones or reuses a checkout, runs `make`), it isn't
# pretending to be a binary-release installer it isn't.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/koshirfan1/Kosh/main/install.sh | bash
#   # or, from an existing local checkout:
#   ./install.sh
#
# What it does, in order:
#   1. Checks for gcc/cc and make (refuses to silently half-install).
#   2. Checks for libsqlite3 dev headers — Kosh's sqlite bridge needs
#      them to compile; this script does NOT sudo-install anything on
#      your behalf (a curl-piped script silently running sudo apt
#      install is exactly the kind of thing that shouldn't happen
#      without you reading it first) — it tells you the command for
#      your platform and stops.
#   3. Uses the current directory if it looks like a Kosh checkout
#      (has makefile + src/main.c), otherwise clones the repo into
#      ~/.kosh/src.
#   4. Runs `make`.
#   5. Runs `make install` (installs to ~/.kosh/bin — no sudo needed).
#   6. Adds ~/.kosh/bin to PATH in whichever shell rc file matches
#      your current shell, if it isn't there already.
#   7. Verifies with `kosh --version`.
#
# Safe to re-run — every step is idempotent (re-cloning is skipped if
# the checkout already exists, the PATH line is only added once).

set -euo pipefail

KOSH_HOME="${KOSH_HOME:-$HOME/.kosh}"
KOSH_SRC="$KOSH_HOME/src"
KOSH_REPO="https://github.com/koshirfan1/Kosh.git"

info()  { printf '  %s\n' "$1"; }
warn()  { printf '  \033[33m%s\033[0m\n' "$1" >&2; }
fail()  { printf '  \033[31mError: %s\033[0m\n' "$1" >&2; exit 1; }

echo "Kosh installer"
echo "=============="

# ── 1. Compiler + make ──────────────────────────────────────────────
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
    fail "no C compiler found (need gcc or cc). Install one first:
    Debian/Ubuntu:  sudo apt install build-essential
    macOS:          xcode-select --install
    Fedora:         sudo dnf groupinstall \"Development Tools\""
fi
if ! command -v make >/dev/null 2>&1; then
    fail "make not found. Install it the same way as the compiler above."
fi
info "Found a C compiler and make."

# ── 2. libsqlite3 dev headers ───────────────────────────────────────
# Header-only check (not a full compile) — good enough to catch the
# common "forgot to install -dev" case before a confusing build error.
if ! echo '#include <sqlite3.h>' | ${CC:-cc} -E - >/dev/null 2>&1; then
    fail "sqlite3.h not found — Kosh's sqlite bridge needs the dev headers.
    Debian/Ubuntu:  sudo apt install libsqlite3-dev
    macOS (brew):   brew install sqlite3
    Fedora:         sudo dnf install sqlite-devel
    Then re-run this script."
fi
info "Found sqlite3 dev headers."

# ── 3. Get the source ───────────────────────────────────────────────
if [ -f "./makefile" ] && [ -f "./src/main.c" ]; then
    info "Running from an existing Kosh checkout — building in place."
    BUILD_DIR="."
elif [ -d "$KOSH_SRC/.git" ]; then
    info "Reusing existing checkout at $KOSH_SRC — pulling latest."
    git -C "$KOSH_SRC" pull --ff-only
    BUILD_DIR="$KOSH_SRC"
else
    if ! command -v git >/dev/null 2>&1; then
        fail "git not found and no local checkout detected — install git or run this script from inside a Kosh checkout."
    fi
    info "Cloning Kosh into $KOSH_SRC ..."
    mkdir -p "$KOSH_HOME"
    git clone --depth 1 "$KOSH_REPO" "$KOSH_SRC"
    BUILD_DIR="$KOSH_SRC"
fi

# ── 4 & 5. Build and install ────────────────────────────────────────
info "Building (make) ..."
( cd "$BUILD_DIR" && make )
info "Installing to $KOSH_HOME/bin (no sudo needed) ..."
( cd "$BUILD_DIR" && make install PREFIX="$KOSH_HOME" )

# ── 6. PATH setup ────────────────────────────────────────────────────
BIN_DIR="$KOSH_HOME/bin"
PATH_LINE="export PATH=\"$BIN_DIR:\$PATH\""

# Pick the rc file matching the current interactive shell, falling
# back to .profile if the shell isn't recognized. $SHELL reflects the
# user's login shell, which is the right thing to check here (not
# $0/$BASH_VERSION, which would reflect whatever's running this
# script, not necessarily the user's usual shell).
case "${SHELL:-}" in
    */zsh)  RC_FILE="$HOME/.zshrc" ;;
    */bash) RC_FILE="$HOME/.bashrc" ;;
    *)      RC_FILE="$HOME/.profile" ;;
esac

if echo ":${PATH:-}:" | grep -q ":$BIN_DIR:"; then
    info "$BIN_DIR is already on PATH."
elif [ -f "$RC_FILE" ] && grep -qF "$BIN_DIR" "$RC_FILE" 2>/dev/null; then
    info "$RC_FILE already references $BIN_DIR — not adding it again."
    info "(If it's still not on PATH, restart your shell.)"
else
    printf '\n# Added by Kosh'"'"'s install.sh\n%s\n' "$PATH_LINE" >> "$RC_FILE"
    info "Added $BIN_DIR to PATH in $RC_FILE."
fi

# ── 7. Verify ────────────────────────────────────────────────────────
echo ""
if "$BIN_DIR/kosh" --version >/dev/null 2>&1; then
    echo "✓ Installed: $("$BIN_DIR/kosh" --version)"
    echo ""
    echo "Restart your shell (or run: source $RC_FILE) and then:"
    echo "    kosh --version"
    echo "    kosh path/to/script.kh"
else
    warn "Build finished but $BIN_DIR/kosh --version didn't run cleanly — something's off."
    warn "Try running it directly to see the error: $BIN_DIR/kosh --version"
    exit 1
fi
