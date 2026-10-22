# Kosh Dotenv Package — v1.0.0

**Package**: `dotenv`  
**Installation**: `kh install dotenv`

## Overview

The `dotenv` package loads environment variables from `.env` files into Kosh maps. Supports `KEY=VALUE` format, quoted values, comments (`#`), and whitespace trimming.

## Installation

```bash
kh install dotenv
```

```kosh
import "dotenv"
```

---

## Functions

```kosh
load_env(path)          # Load .env file → map of key-value pairs
env_get(env_map, key)   # Get value from env map, or nil if missing
```

#### Example

Given a `.env` file:
```
# Database configuration
DB_HOST=localhost
DB_PORT=5432
DB_NAME=kosh_app
API_KEY="sk-abc123"
```

```kosh
let env = load_env(".env")
let host = env_get(env, "DB_HOST")     # "localhost"
let port = env_get(env, "DB_PORT")     # "5432"
let key  = env_get(env, "API_KEY")     # "sk-abc123"
let missing = env_get(env, "NONEXIST") # nil
```

---

## Complete Example

```kosh
import "dotenv"

# Load configuration
let config = load_env("config.env")

# Access values
let db_host = env_get(config, "DB_HOST")
let db_port = env_get(config, "DB_PORT")
let debug   = env_get(config, "DEBUG")

if debug == "true":
    print "Debug mode enabled"

print "Connecting to " + db_host + ":" + db_port
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
