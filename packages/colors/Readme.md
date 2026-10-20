# Kosh Colors Package — v1.0.0

**Package**: `colors`  
**Installation**: `kh install colors`

## Overview

The `colors` package provides terminal ANSI color output for Kosh scripts. It includes standard colors, bright variants, background colors, text styles (bold, dim, underline), and convenience print functions with automatic prefix labels.

## Installation

```bash
kh install colors
```

```kosh
import "colors"
```

---

## Color Constants

### Standard Foreground Colors

| Constant | ANSI Code | Color |
|----------|-----------|-------|
| `CLR_BLACK` | `\x1b[30m` | Black |
| `CLR_RED` | `\x1b[31m` | Red |
| `CLR_GREEN` | `\x1b[32m` | Green |
| `CLR_YELLOW` | `\x1b[33m` | Yellow |
| `CLR_BLUE` | `\x1b[34m` | Blue |
| `CLR_MAGENTA` | `\x1b[35m` | Magenta |
| `CLR_CYAN` | `\x1b[36m` | Cyan |
| `CLR_WHITE` | `\x1b[37m` | White |

### Bright Foreground Colors

| Constant | ANSI Code | Color |
|----------|-----------|-------|
| `CLR_BRED` | `\x1b[91m` | Bright Red |
| `CLR_BGREEN` | `\x1b[92m` | Bright Green |
| `CLR_BYELLOW` | `\x1b[93m` | Bright Yellow |
| `CLR_BBLUE` | `\x1b[94m` | Bright Blue |
| `CLR_BCYAN` | `\x1b[96m` | Bright Cyan |
| `CLR_BWHITE` | `\x1b[97m` | Bright White |

### Background Colors

| Constant | ANSI Code | Color |
|----------|-----------|-------|
| `BG_RED` | `\x1b[41m` | Red Background |
| `BG_GREEN` | `\x1b[42m` | Green Background |
| `BG_YELLOW` | `\x1b[43m` | Yellow Background |
| `BG_BLUE` | `\x1b[44m` | Blue Background |
| `BG_CYAN` | `\x1b[46m` | Cyan Background |
| `BG_WHITE` | `\x1b[47m` | White Background |

### Text Styles

| Constant | ANSI Code | Effect |
|----------|-----------|--------|
| `CLR_RESET` | `\x1b[0m` | Reset all styles |
| `CLR_BOLD` | `\x1b[1m` | Bold text |
| `CLR_DIM` | `\x1b[2m` | Dim text |
| `CLR_UNDER` | `\x1b[4m` | Underlined text |

---

## Color Functions

### Basic Color Functions

Each function wraps text in the corresponding ANSI color code and appends a reset.

```kosh
red(text)        # Red text
green(text)      # Green text
yellow(text)     # Yellow text
blue(text)       # Blue text
magenta(text)    # Magenta text
cyan(text)       # Cyan text
white(text)      # White text
```

### Bright Color Functions

```kosh
bred(text)       # Bright red text
bgreen(text)     # Bright green text
byellow(text)    # Bright yellow text
bcyan(text)      # Bright cyan text
```

### Style Functions

```kosh
bold(text)       # Bold text
dim(text)        # Dim text
underline(text)  # Underlined text
```

### Generic Color Function

```kosh
color(text, clr)  # Apply any color/style constant to text
```

#### Examples

```kosh
print color("custom color", CLR_BLUE + CLR_BOLD)  # Bold blue text
print color("red bg", BG_RED + CLR_WHITE)          # White text on red background
```

---

## Print Functions

These functions print directly with colored output and automatic prefix labels.

```kosh
print_red(text)       # Print red text
print_green(text)     # Print green text
print_yellow(text)    # Print yellow text
print_blue(text)      # Print blue text
print_cyan(text)      # Print cyan text
print_bold(text)      # Print bold text
print_success(text)   # Print "OK <text>" in bright green
print_error(text)     # Print "ERR <text>" in bright red
print_warn(text)      # Print "WARN <text>" in bright yellow
print_info(text)      # Print "INFO <text>" in cyan
```

#### Examples

```kosh
print_success("Tests passed")     # OK Tests passed (bright green)
print_error("File not found")     # ERR File not found (bright red)
print_warn("Deprecated API")      # WARN Deprecated API (bright yellow)
print_info("Server started")      # INFO Server started (cyan)
```

---

## Complete Example

```kosh
import "colors"

# Basic colors
print red("error occurred")
print green("build passed")
print yellow("warning: low memory")
print blue("information")
print bold("important message")

# Bright colors
print bred("critical error")
print bgreen("all systems go")

# Print helpers with labels
print_success("Tests passed")
print_error("File not found")
print_warn("Deprecated API")
print_info("Server started on port 8080")

# Custom combinations
print color("URGENT", CLR_BRED + CLR_BOLD + BG_YELLOW)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |