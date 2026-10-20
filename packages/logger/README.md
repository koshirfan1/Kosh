# Kosh Logger Package — v1.0.0

**Author**: Irfan Khan  
**Package**: `logger`  
**Installation**: `kh install logger`

## Overview

The `logger` package provides structured logging for Kosh applications with multiple severity levels, console output, file saving, and query capabilities. Supports DEBUG, INFO, WARN, ERROR, and NONE levels.

## Installation

```bash
kh install logger
```

```kosh
import "logger"
```

---

## Log Levels

| Constant | Value | Description |
|----------|-------|-------------|
| `LOG_DEBUG` | 0 | Detailed debug information |
| `LOG_INFO` | 1 | General information (default) |
| `LOG_WARN` | 2 | Warning conditions |
| `LOG_ERROR` | 3 | Error conditions |
| `LOG_NONE` | 4 | Suppress all output |

---

## Creating a Logger

```kosh
logger_new(name)                # Create a new logger
logger_set_level(log, level)    # Set minimum log level
logger_silent(log)             # Suppress console output
```

#### Example

```kosh
let log = logger_new("MyApp")
log = logger_set_level(log, LOG_DEBUG)  # Show everything

# Silent mode (record only, no console output)
let silent_log = logger_silent(logger_new("BatchJob"))
```

---

## Logging Functions

```kosh
log_debug(log, msg)     # Log at DEBUG level
log_info(log, msg)      # Log at INFO level
log_warn(log, msg)      # Log at WARN level
log_error(log, msg)     # Log at ERROR level
log_divider(log)        # Log a divider line
log_kv(log, key, value) # Log a key=value pair
log_map(log, label, m)  # Log all entries in a map
```

#### Examples

```kosh
log = log_debug(log, "Starting initialization")
log = log_info(log, "Server started on port 8080")
log = log_warn(log, "High memory usage: 85%")
log = log_error(log, "Connection failed: timeout")
log = log_divider(log)
log = log_kv(log, "version", "2.0.0")

let config = {"host": "localhost", "port": 8080}
log = log_map(log, "Server config", config)
```

Console output format:
```
[MyApp] [INFO] Server started on port 8080
[MyApp] [WARN] High memory usage: 85%
[MyApp] [ERROR] Connection failed: timeout
[MyApp] [INFO] version = 2.0.0
[MyApp] [INFO] Server config:
[MyApp] [INFO]   host: localhost
[MyApp] [INFO]   port: 8080
```

---

## Querying Logs

```kosh
logger_errors(log)      # Get all ERROR entries → array
logger_warnings(log)    # Get all WARN entries → array
logger_has_errors(log)  # Check if any errors occurred → bool
logger_count(log)       # Get total entry count → number
```

#### Example

```kosh
if logger_has_errors(log):
    print "Found errors:"
    for e in logger_errors(log):
        print "  - " + e["message"]
```

---

## Saving Logs

```kosh
logger_to_string(log)   # Convert all entries to string
logger_save(log, path)  # Save log entries to file
logger_summary(log)     # Print a formatted summary
```

#### Examples

```kosh
# Save to file
logger_save(log, "app.log")

# Print summary
logger_summary(log)
```

Output of `logger_summary()`:
```
── Logger summary: MyApp ──
  Total entries : 42
  Warnings      : 3
  Errors        : 1
```

---

## Complete Example

```kosh
import "logger"

# Create logger
let log = logger_new("WebServer")
log = logger_set_level(log, LOG_DEBUG)

# Log some events
log = log_info(log, "Starting web server...")
log = log_kv(log, "port", 3000)
log = log_debug(log, "Loading routes...")
log = log_warn(log, "SSL certificate not configured")

# Simulate request logging
fn log_request(log, method, path, status):
    if status >= 500:
        return log_error(log, method + " " + path + " → " + str(status))
    elif status >= 400:
        return log_warn(log, method + " " + path + " → " + str(status))
    return log_info(log, method + " " + path + " → " + str(status))

log = log_request(log, "GET", "/api/users", 200)
log = log_request(log, "POST", "/api/login", 401)
log = log_request(log, "GET", "/api/error", 500)

# Summary and save
logger_summary(log)
logger_save(log, "server.log")
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
