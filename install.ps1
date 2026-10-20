# Khan installer for Windows — mirrors the "irm | iex" experience of
# rustup, deno, etc. on Windows, and persists PATH the same way the
# official Python/Node.js Windows installers do: writing to the
# CURRENT USER's Environment registry key via
# [Environment]::SetEnvironmentVariable(..., "User"), NOT just
# $env:PATH for the current session (which would vanish the moment
# this window closes — that's the single most common way a "PATH
# setup" script for Windows ends up not actually working).
#
# There's no pre-built binary release yet (see ROADMAP_STATUS_UPDATED.md
# — v1.0 hasn't shipped), so like install.sh, this builds from source.
# It needs a GCC toolchain on PATH already (MSYS2/MinGW-w64) — it does
# NOT install one for you; silently installing a multi-hundred-MB
# toolchain from a piped script is a bigger, riskier action than a
# PATH edit, and deserves you reading a message and deciding, not a
# script deciding for you.
#
# Usage (PowerShell):
#   irm https://raw.githubusercontent.com/khandev1211-cpu/Khan/main/install.ps1 | iex
#   # or, from an existing local checkout:
#   .\install.ps1
#
# What it does, in order:
#   1. Checks for gcc and make on PATH (refuses to silently half-install).
#   2. Checks for sqlite3.h reachability the same way install.sh does
#      (a header-only compile check, not a full build).
#   3. Uses the current directory if it looks like a Khan checkout,
#      otherwise clones the repo into $env:USERPROFILE\.khan\src.
#   4. Runs `make`.
#   5. Copies khan.exe/kh.exe into $env:USERPROFILE\.khan\bin.
#   6. Adds that folder to the CURRENT USER's PATH (persisted via the
#      registry, survives closing this window) — the same mechanism
#      Python's "Add to PATH" installer checkbox uses under the hood.
#   7. Verifies with `khan --version`.
#
# Safe to re-run — every step is idempotent.

$ErrorActionPreference = "Stop"

$KhanHome = if ($env:KHAN_HOME) { $env:KHAN_HOME } else { Join-Path $env:USERPROFILE ".khan" }
$KhanSrc  = Join-Path $KhanHome "src"
$KhanBin  = Join-Path $KhanHome "bin"
$KhanRepo = "https://github.com/khandev1211-cpu/Khan.git"

function Info($msg) { Write-Host "  $msg" }
function Warn($msg) { Write-Host "  $msg" -ForegroundColor Yellow }
function Fail($msg) { Write-Host "  Error: $msg" -ForegroundColor Red; exit 1 }

Write-Host "Khan installer (Windows)"
Write-Host "========================"

# -- 1. Compiler + make --------------------------------------------------
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Fail "gcc not found on PATH. Khan needs a GCC toolchain (MSYS2/MinGW-w64) to build from source.`n`n    Install MSYS2 from https://www.msys2.org/, then from an MSYS2 MinGW64 shell:`n        pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make mingw-w64-x86_64-sqlite3`n    and make sure C:\msys64\mingw64\bin is on your PATH before re-running this script."
}
if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
    Fail "make not found on PATH. Install it the same way as gcc above (mingw-w64-x86_64-make)."
}
Info "Found gcc and make on PATH."

# -- 2. sqlite3 dev headers ----------------------------------------------
$sqliteCheckDir = Join-Path $env:TEMP "khan_sqlite_check_$PID"
New-Item -ItemType Directory -Path $sqliteCheckDir -Force | Out-Null
$checkFile = Join-Path $sqliteCheckDir "check.c"
Set-Content -Path $checkFile -Value "#include <sqlite3.h>`nint main(void){return 0;}"
$sqliteOk = $true
try {
    & gcc -c $checkFile -o (Join-Path $sqliteCheckDir "check.o") 2>$null
    if ($LASTEXITCODE -ne 0) { $sqliteOk = $false }
} catch {
    $sqliteOk = $false
}
Remove-Item -Recurse -Force $sqliteCheckDir -ErrorAction SilentlyContinue
if (-not $sqliteOk) {
    Fail "sqlite3.h not found -- Khan's sqlite bridge needs the dev headers.`n`n    From an MSYS2 MinGW64 shell:`n        pacman -S mingw-w64-x86_64-sqlite3`n    Then re-run this script."
}
Info "Found sqlite3 dev headers."

# -- 3. Get the source ----------------------------------------------------
if ((Test-Path ".\makefile") -and (Test-Path ".\src\main.c")) {
    Info "Running from an existing Khan checkout -- building in place."
    $BuildDir = "."
} elseif (Test-Path (Join-Path $KhanSrc ".git")) {
    Info "Reusing existing checkout at $KhanSrc -- pulling latest."
    git -C $KhanSrc pull --ff-only
    $BuildDir = $KhanSrc
} else {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Fail "git not found and no local checkout detected -- install git or run this script from inside a Khan checkout."
    }
    Info "Cloning Khan into $KhanSrc ..."
    New-Item -ItemType Directory -Path $KhanHome -Force | Out-Null
    git clone --depth 1 $KhanRepo $KhanSrc
    $BuildDir = $KhanSrc
}

# -- 4 & 5. Build and install ----------------------------------------------
Info "Building (make) ..."
Push-Location $BuildDir
try {
    & make
    if ($LASTEXITCODE -ne 0) { Fail "make failed -- see output above." }
} finally {
    Pop-Location
}

Info "Installing to $KhanBin ..."
New-Item -ItemType Directory -Path $KhanBin -Force | Out-Null
Copy-Item (Join-Path $BuildDir "khan.exe") (Join-Path $KhanBin "khan.exe") -Force
Copy-Item (Join-Path $BuildDir "kh.exe")   (Join-Path $KhanBin "kh.exe")   -Force

# -- 6. Persist PATH for the CURRENT USER ----------------------------------
# This is the actual "installs like other languages" part: writing to
# the registry via SetEnvironmentVariable(..., "User") is exactly what
# survives closing this terminal and opening a new one -- unlike
# `$env:PATH += ...`, which only affects the current process and every
# child of it, and is gone the moment this window closes. New terminal
# windows opened after this pick up the new PATH automatically; windows
# already open when this runs won't see it until reopened (Windows
# itself works this way for every installer, not something specific
# to this script).
$currentUserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($null -eq $currentUserPath) { $currentUserPath = "" }
$pathEntries = $currentUserPath -split ";" | Where-Object { $_ -ne "" }

if ($pathEntries -contains $KhanBin) {
    Info "$KhanBin is already on your User PATH."
} else {
    $newPath = if ($currentUserPath.Trim() -eq "") { $KhanBin } else { "$currentUserPath;$KhanBin" }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    # Also update the current session's PATH so `khan --version` below
    # (and any use of `khan` for the rest of THIS terminal session)
    # works immediately, without waiting for a new window.
    $env:Path = "$env:Path;$KhanBin"
    Info "Added $KhanBin to your User PATH (persisted -- new terminal windows will have it automatically)."
}

# -- 7. Verify --------------------------------------------------------------
Write-Host ""
$khanExe = Join-Path $KhanBin "khan.exe"
try {
    $version = & $khanExe --version
    Write-Host "Installed: $version" -ForegroundColor Green
    Write-Host ""
    Write-Host "This terminal window already has khan on PATH. New windows will too."
    Write-Host "    khan --version"
    Write-Host "    khan path\to\script.kh"
} catch {
    Warn "Build finished but $khanExe --version didn't run cleanly -- something's off."
    Warn "Try running it directly to see the error: $khanExe --version"
    exit 1
}
