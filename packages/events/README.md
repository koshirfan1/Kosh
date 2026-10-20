# Kosh Events Package — v1.0.0

**Author**: Irfan Khan  
**Package**: `events`  
**Installation**: `kh install events`

## Overview

The `events` package provides an event emitter system for Kosh, designed for use with the Kosh Engine (game events) and Kosh Studio (UI events). It supports publish/subscribe patterns with `on`, `once`, `off`, and `emit` semantics, plus event history tracking.

## Installation

```bash
kh install events
```

```kosh
import "events"
```

---

## Creating an Event Bus

```kosh
ev_bus()                # Create a new event bus
ev_bus_logged()         # Create a bus that prints events as they occur
```

#### Example

```kosh
let bus = ev_bus()
let logged_bus = ev_bus_logged()  # Prints: [event] event_name → data
```

---

## Subscribing to Events

```kosh
ev_on(bus, event, handler)          # Subscribe to event (handler receives data)
ev_once(bus, event, handler)        # Subscribe for one-time notification
ev_off(bus, event)                  # Unsubscribe all handlers for event
ev_off_all(bus)                     # Remove all subscriptions
```

Handlers are functions that take a single `data` argument.

#### Examples

```kosh
fn on_collision(data):
    print "Collision detected between " + data["entity"] + " and " + data["other"]

fn on_update(data):
    print "Frame update: " + str(data)

# Subscribe
bus = ev_on(bus, "collision", on_collision)
bus = ev_on(bus, "update", on_update)
bus = ev_once(bus, "startup", fn(data): print "First run: " + str(data))

# Unsubscribe
bus = ev_off(bus, "update")
bus = ev_off_all(bus)  # Remove everything
```

---

## Emitting Events

```kosh
ev_emit(bus, event, data)           # Emit an event with data
ev_emit_many(bus, events)           # Emit multiple events from [{event, data}, ...]
```

#### Examples

```kosh
# Single event
bus = ev_emit(bus, "collision", {"entity": "player", "other": "wall"})

# Multiple events
let events_list = [
    {"event": "start", "data": {"time": 0}},
    {"event": "update", "data": {"frame": 1}},
    {"event": "update", "data": {"frame": 2}}
]
bus = ev_emit_many(bus, events_list)
```

---

## Inspecting Event History

Each emitted event is recorded in the bus history with a timestamp.

```kosh
ev_has_listeners(bus, event)    # Check if event has subscribers → bool
ev_history(bus)                 # Get full event history → array
ev_history_for(bus, event)      # Get history for specific event → array
ev_count(bus, event)            # Count occurrences of event → number
ev_last(bus, event)             # Get last occurrence of event → map or nil
ev_clear_history(bus)           # Clear all history
```

#### Examples

```kosh
print ev_has_listeners(bus, "collision")  # true
print ev_count(bus, "update")             # 2
let last = ev_last(bus, "update")
print last["data"]["frame"]                # 2
bus = ev_clear_history(bus)
```

---

## Game Engine Preset Events

The package includes predefined event name constants for game development:

| Constant | Value | Purpose |
|----------|-------|---------|
| `EV_UPDATE` | `"update"` | Per-frame update |
| `EV_START` | `"start"` | Game/entity start |
| `EV_DESTROY` | `"destroy"` | Entity destruction |
| `EV_COLLISION` | `"collision"` | Collision detection |
| `EV_TRIGGER` | `"trigger"` | Trigger zone activation |
| `EV_INPUT` | `"input"` | General input event |
| `EV_MOUSE` | `"mouse"` | Mouse events |
| `EV_KEY` | `"key"` | Keyboard events |
| `EV_SCENE_LOAD` | `"scene_load"` | Scene loading |
| `EV_SCENE_EXIT` | `"scene_exit"` | Scene exiting |
| `EV_ASSET_LOAD` | `"asset_load"` | Asset loading |
| `EV_AI_DONE` | `"ai_done"` | AI processing complete |

---

## Complete Example

```kosh
import "events"

# Create bus
let bus = ev_bus()

# Define handlers
fn on_score(data):
    print "Score changed to " + str(data["score"])

fn on_level_up(data):
    print "Level up! Now level " + str(data["level"])

# Subscribe
bus = ev_on(bus, "score_change", on_score)
bus = ev_once(bus, "game_start", fn(d): print "Game started at " + str(d["time"]))

# Emit events
bus = ev_emit(bus, "game_start", {"time": clock()})
bus = ev_emit(bus, "score_change", {"score": 100})
bus = ev_emit(bus, "score_change", {"score": 200})

# Inspect
print "Score changes recorded: " + str(ev_count(bus, "score_change"))
print "Has listeners: " + str(ev_has_listeners(bus, "score_change"))

# Clean up
bus = ev_off_all(bus)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
