# Installing and building Kosh

[← Back to the README](../README.md)

## Install

**Linux / macOS:**
```bash
curl -fsSL https://raw.githubusercontent.com/koshirfan1/Kosh/main/install.sh | bash
```

**Windows (PowerShell):**
```powershell
irm https://raw.githubusercontent.com/koshirfan1/Kosh/main/install.ps1 | iex
```
Needs a GCC toolchain on `PATH` already (MSYS2/MinGW-w64) — the script checks for one and tells you exactly what to install if it's missing, rather than silently installing a toolchain for you. **Caveat, stated plainly**: this script was written and reviewed carefully but has not been run on an actual Windows machine in the environment it was authored in (no such access there) — same class of caveat this project's CI workflow already carries for its own Windows steps. If something in it doesn't work as described, that's a bug — please open an issue with what happened.

**Windows (GUI installer, alternative to the PowerShell script above):** [kosh-installer.iss](../kosh-installer.iss) is an [Inno Setup](https://jrsoftware.org/isinfo.php) script that produces a proper double-click `kosh-setup.exe` — a setup wizard with an **"Add Kosh to PATH" checkbox** (checked by default), a Start Menu entry, and a normal Add/Remove Programs uninstaller, closer to Python's own official Windows installer experience than a terminal one-liner is. It packages already-built `kosh.exe`/`kh.exe` rather than compiling them itself (see the comment at the top of that file for the exact build-then-compile steps). [.github/workflows/release.yml](../.github/workflows/release.yml) automates exactly that build-then-compile sequence and can publish the result to Releases (see just below) — until a tagged release actually exists there, building `kosh-setup.exe` yourself via the free Inno Setup Compiler is the only way to get one. Same not-yet-verified-on-real-Windows caveat as `install.ps1` above applies here too.

There's no *published* pre-built binary release yet (see the [roadmap](roadmap.md) — v1.0 hasn't shipped) — [.github/workflows/release.yml](../.github/workflows/release.yml) can build one (`kosh.exe`, `kh.exe`, and a compiled `kosh-setup.exe` from the Inno Setup script above) and publish it to this repo's [Releases page](https://github.com/koshirfan1/Kosh/releases) automatically whenever a version tag (`v0.010.0`-style) is pushed, but that hasn't been done yet either. Once it has, downloading a release asset from there is the fastest path on Windows — no compiler needed. Until then, both scripted installers below build from source rather than downloading a binary, the way `rustup`/`deno`'s installers do when there's nothing prebuilt for your platform yet. Neither one runs `sudo`/an elevated prompt on your behalf — they check for what they need and tell you the exact command for your platform if something's missing. Read [install.sh](../install.sh) / [install.ps1](../install.ps1) before piping either into a shell, same as you should for any curl-pipe installer — that's not Kosh-specific advice.

After it finishes — restart your shell on Linux/macOS (or `source` the rc file it mentions); on Windows, `kosh` is already on `PATH` in the same PowerShell window, new windows get it automatically too:

```bash
kosh --version
kosh                    # starts an interactive REPL — see "Interactive REPL" below
kosh path/to/script.kh

# Install packages
kh install math
kh install strings
kh install colors
kh install requests
kh install postman
```

```kosh
import "math"
import "colors"

print green("Kosh is ready!")
print math_sqrt(144)
```

**Prefer to build it yourself, or the installer doesn't fit your setup?**

```bash
git clone https://github.com/koshirfan1/Kosh.git
cd Kosh
make                    # builds kosh(.exe) and kh(.exe) in the current directory
make install            # Linux/macOS only — installs both to ~/.kosh/bin, same as install.sh's last step.
                        # On Windows, use install.ps1 instead (`make install` prints why and stops).
```

Without `make install`, run the binaries directly as `./kosh script.kh` (Linux/macOS) or `kosh.exe script.kh` (Windows) from inside the checkout.

## Building from Source

### Prerequisites

- **GCC** (C11) — MinGW64 on Windows, or any POSIX GCC
- **GNU Make**
- **libtesseract** (+ its trained language data) — **optional**, only for the opt-in OCR build: `make OCR=1`. The default `make` never needs it. See [docs/ocr.md](ocr.md) for the exact install command per platform (apt/Homebrew/MSYS2).
- **libsqlite3** (dev headers) — required by the `sqlite` package's native bridge. `apt install libsqlite3-dev` / `brew install sqlite3` / MSYS2 `mingw-w64-x86_64-sqlite3`. The build fails without it (missing `sqlite3.h`) rather than silently skipping SQLite support.
- **Windows only**: links `-lwinhttp`, `-lshell32`, and `-lsqlite3` (the first two ship with Windows; sqlite3 does not)
- **Linux/macOS**: links `-lm` and `-lsqlite3`, uses `curl` for HTTP

### Build

```bash
git clone https://github.com/koshirfan1/Kosh.git
cd Kosh
make
```

```bash
make OCR=1              # optional: also build the ocr package's native Tesseract bridge
```

Produces `kosh.exe` (the compiler+VM runtime) and `kh.exe` (package manager) on Windows, or `kosh` and `kh` on Linux/macOS.

```bash
make clean   # remove build artifacts
make install # Linux/macOS: install kosh and kh to ~/.kosh/bin (override with PREFIX=/your/path)
make uninstall # remove them again
```

On Windows, `make install`/`make uninstall` print a pointer to `install.ps1` instead of doing the copy themselves — see that script's own comments for why PATH persistence needs the registry (`[Environment]::SetEnvironmentVariable`), not something a makefile target can do portably.

The [install section](#install) above's `install.sh` (Linux/macOS) / `install.ps1` (Windows) does the `git clone` + `make` + `make install` sequence above for you, plus PATH setup — this section is the manual, step-by-step equivalent if you'd rather not pipe a script into a shell, or need to customize the build (e.g. `LLM=1` for the optional llama.cpp bridge — see the makefile).

### Exit Codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `64` | Wrong number of arguments |
| `65` | Parse/syntax error |
| `70` | Runtime error |
| `74` | I/O error (file not found) |

### CLI flags

| Flag | Meaning |
|---|---|
| `kosh script.kh [args...]` | Run a script; extra args are available inside it as the global array `argv` |
| `kosh` (no arguments) | Starts an interactive REPL — see below |
| `kosh --version` / `-v` | Print the version and exit |
| `kosh --help` / `-h` | Print usage and exit |

### Interactive REPL

Running `kosh` with no arguments — the same trigger `python` uses — starts an interactive session: type a line, see it run immediately, keep going.

```
$ kosh
Kosh 0.016.0 — interactive mode. Type 'exit' or press Ctrl+D to quit.
kosh> let x = 10
kosh> x * 5
50
kosh> fn square(n):
...     return n * n
...
kosh> square(7)
49
kosh> exit
```

A few things worth knowing about how it behaves:
- **Variables persist across lines** — one shared VM instance runs every line you type, so `let x = 10` on one line is still visible as `x` on the next, the whole point of a REPL over running one-off scripts.
- **A bare expression auto-prints its value** (`x * 5` above shows `50` with no explicit `print`) — same convention as Python's REPL. Statements that don't produce a value (`let`, `if`, `fn`, …) print nothing, same as always.
- **Multi-line blocks**: Kosh is indentation-based like Python, so a line ending in `:` (an `fn`/`if`/`while`/`try` header) switches the prompt to `...` and keeps reading indented lines until a **blank line** closes the block, then runs the whole thing at once. This is a simpler rule than Python's own indentation-tracking REPL, but it means the same thing in practice: finish a block, then hit Enter on an empty line.
- **A runtime error doesn't end the session** — it prints and the REPL keeps going with all your variables intact, the same way a real interactive interpreter should behave, rather than one mistake forcing a restart.
- Type `exit`, `quit` (with or without `()`), or press Ctrl+D to leave.
