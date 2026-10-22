# Kosh Argparse Package — v1.0.0

**Package**: `argparse`  
**Installation**: `kh install argparse`

## Overview

The `argparse` package provides command-line argument parsing for Kosh scripts. Supports flags (boolean) and options (with values), automatic `--help`/`-h` generation, and formatted help text.

## Installation

```bash
kh install argparse
```

```kosh
import "argparse"
```

---

## Creating a Parser

```kosh
arg_parser(description)     # Create a new argument parser
```

#### Example

```kosh
let parser = arg_parser("My script - processes data files")
```

---

## Defining Arguments

```kosh
add_arg(parser, flag, default_val, help)    # Add option with value (e.g. --output file.txt)
add_flag(parser, flag, help)                # Add boolean flag (e.g. --verbose)
```

#### Example

```kosh
parser = add_arg(parser, "output", "out.txt", "Output file path")
parser = add_arg(parser, "port", 8080, "Server port")
parser = add_flag(parser, "verbose", "Enable verbose logging")
parser = add_flag(parser, "debug", "Enable debug mode")
```

---

## Parsing Arguments

```kosh
parse_args(parser, args_list)   # Parse arguments → result map
arg_help(parser)                # Print formatted help text
```

The parser uses Kosh's built-in `argv` global variable. Pass `argv` to `parse_args()`.

#### Example

```kosh
let args = parse_args(parser, argv)

if args["help"]:
    arg_help(parser)
    exit(0)

let output = args["output"]
let port = args["port"]
let verbose = args["verbose"]
```

---

## Complete Example

```kosh
import "argparse"

# Define parser
let parser = arg_parser("File processor v1.0")
parser = add_arg(parser, "input", "data.txt", "Input file path")
parser = add_arg(parser, "output", "result.txt", "Output file path")
parser = add_flag(parser, "verbose", "Show detailed output")
parser = add_flag(parser, "compress", "Compress output")

# Parse (argv comes from Kosh's built-in)
let args = parse_args(parser, argv)

# Show help if requested
if args["help"]:
    arg_help(parser)
    exit(0)

# Use arguments
print "Input: " + args["input"]
print "Output: " + args["output"]
if args["verbose"]:
    print "Verbose mode enabled"
if args["compress"]:
    print "Compression enabled"
```

Command line usage:
```bash
kosh script.kh --input mydata.txt --output results.txt --verbose
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |