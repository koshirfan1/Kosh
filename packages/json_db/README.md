# Khan JSON Database Package — v1.0.0

**Package**: `json_db`  
**Installation**: `kh install json_db`

## Overview

The `json_db` package provides a simple NoSQL database using JSON files. It stores data as key-value maps persisted to disk in JSON format.

## Installation

```bash
kh install json_db
```

```khan
import "json_db"
```

---

## Functions

```khan
db_open(path)               # Open/create a JSON database file → db map
db_get(db, key, default)    # Get value by key (or default)
db_set(db, key, value)      # Set value by key
db_has(db, key)             # Check if key exists → bool
db_delete(db, key)          # Delete a key
db_save(db)                 # Save database to disk → bool
```

#### Example

```khan
# Open database
let db = db_open("mydb.json")

# Read values
let name = db_get(db, "name", "default_name")
let count = db_get(db, "count", 0)

# Modify
db = db_set(db, "name", "Khan App")
db = db_set(db, "count", count + 1)
db = db_set(db, "last_run", clock())

# Save
db_save(db)
```

---

## Complete Example

```khan
import "json_db"

# Open or create database
let db = db_open("visits.json")

# Get/update visit counter
let visits = db_get(db, "visits", 0)
visits = visits + 1
db = db_set(db, "visits", visits)
db = db_set(db, "last_visit", clock())

# Save changes
db_save(db)

print "This program has been run " + str(visits) + " times"
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
