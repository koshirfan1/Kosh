# Kosh ORM Package — v1.0.0

**Package**: `orm`  
**Installation**: `kh install orm`

## Overview

The `orm` package provides a simple Object-Relational Mapper for JSON databases. It wraps the `json_db` package with model-based CRUD operations.

## Installation

```bash
kh install orm
```

```kosh
import "orm"
```

---

## Functions

```kosh
orm_model(table_name)               # Create a model for a table
orm_create(model, data)             # Insert a record → model
orm_find_all(model)                 # Get all records → array
orm_find_one(model, field, value)   # Find first record matching field=value → map or nil
```

#### Example

```kosh
# Define a model
let users = orm_model("users")

# Create records
users = orm_create(users, {"name": "Alice", "age": 30})
users = orm_create(users, {"name": "Bob", "age": 25})

# Query
let all = orm_find_all(users)
print "Total users: " + str(len(all))

let alice = orm_find_one(users, "name", "Alice")
if alice != nil:
    print "Found: " + alice["name"]
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
