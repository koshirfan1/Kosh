# Khan UUID Package — v1.0.0

**Author**: Irfan Khan  
**Package**: `uuid`  
**Installation**: `kh install uuid`

## Overview

The `uuid` package provides unique identifier generation for Khan. It supports standard UUID v4 format, short IDs, readable named IDs, and sequential counters. Designed for entity IDs, asset IDs, and event IDs in games and applications.

## Installation

```bash
kh install uuid
```

```khan
import "uuid"
```

---

## UUID Generation

```khan
uuid_v4()               # Generate a standard UUID v4 string
uuid_short()            # Generate an 8-character hex short ID
uuid_named(prefix)      # Generate a readable ID with prefix
```

#### Examples

```khan
# Standard UUID v4 (36 chars)
let id = uuid_v4()
print id  # "3f9a1c2b-4e5d-4a8f-9b7c-1d2e3f4a5b6c"

# Short ID (8 chars)
let short = uuid_short()
print short  # "a1b2c3d4"

# Named ID
let player_id = uuid_named("player")
print player_id  # "player_3f9a1c2b"

let asset_id = uuid_named("asset")
print asset_id  # "asset_b7c8d9e0"
```

---

## Sequential Counter

For deterministic, sequential IDs:

```khan
uuid_counter()          # Create a new counter
uuid_next(counter)      # Increment counter
uuid_id(counter)        # Get current counter value
```

#### Example

```khan
let counter = uuid_counter()

let id1 = uuid_id(uuid_next(counter))  # 1
let id2 = uuid_id(uuid_next(counter))  # 2
let id3 = uuid_id(uuid_next(counter))  # 3

print id3  # 3
```

---

## Validation

```khan
uuid_is_valid(s)        # Check if string is valid UUID v4 format → bool
```

#### Example

```khan
let id = uuid_v4()
print uuid_is_valid(id)          # true
print uuid_is_valid("invalid")   # false
```

---

## Complete Example

```khan
import "uuid"

# Different ID types for different purposes
let session_id = uuid_v4()
let entity_id  = uuid_short()
let player1    = uuid_named("player")
let player2    = uuid_named("player")
let item1      = uuid_named("item")

print "Session: " + session_id
print "Entity:  " + entity_id
print "Players: " + player1 + ", " + player2
print "Item:    " + item1

# Sequential IDs for ordered entities
let seq = uuid_counter()
for i in range(5):
    let idx = uuid_id(uuid_next(seq))
    print "Record #" + str(idx)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
