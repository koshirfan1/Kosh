# Kosh Datetime Package — v1.0.0

**Author**: Irfan Khan  
**Package**: `datetime`  
**Installation**: `kh install datetime`

## Overview

The `datetime` package provides date and time utilities for Kosh, built on the `clock()` built-in function. It includes timestamp decomposition, formatting, stopwatch timers, human-readable duration formatting, and sleep helpers.

## Installation

```bash
kh install datetime
```

```kosh
import "datetime"
```

---

## Core Functions

```kosh
dt_now()                    # Get current timestamp (seconds since epoch)
dt_since(past)              # Seconds elapsed since past timestamp
dt_elapsed_ms(past)         # Milliseconds elapsed since past timestamp
```

#### Example

```kosh
let start = dt_now()
# ... do some work ...
print "Elapsed: " + str(dt_since(start)) + " seconds"
print "Elapsed ms: " + str(dt_elapsed_ms(start))
```

---

## Timestamp Decomposition

```kosh
dt_to_parts(ts)             # Decompose timestamp into parts → map
dt_seconds(parts)           # Get seconds component
dt_minutes(parts)           # Get minutes component
dt_hours(parts)             # Get hours component
dt_day_name(parts)          # Get day of week name → string
```

The `dt_to_parts()` function returns a map with:

| Field | Description |
|-------|-------------|
| `timestamp` | Original Unix timestamp |
| `seconds` | Seconds (0–59) |
| `minutes` | Minutes (0–59) |
| `hours` | Hours (0–23) |
| `total_days` | Days since 1970-01-01 |
| `day_of_week` | Day of week (0=Thursday, ..., 6=Wednesday) |

#### Example

```kosh
let p = dt_to_parts(dt_now())
print "Time: " + str(dt_hours(p)) + ":" + str(dt_minutes(p)) + ":" + str(dt_seconds(p))
print "Day: " + dt_day_name(p)
```

---

## Formatting

```kosh
dt_format(ts)               # Format as HH:MM:SS
dt_format_full(ts)          # Format as "DayName HH:MM:SS UTC (day NNN since epoch)"
```

#### Examples

```kosh
let now = dt_now()
print dt_format(now)            # "14:30:45"
print dt_format_full(now)       # "Saturday 14:30:45 UTC (day 20051 since epoch)"
```

---

## Timer / Stopwatch

```kosh
dt_timer_start()            # Create and start a timer
dt_timer_lap(timer)         # Record a lap time
dt_timer_stop(timer)        # Stop the timer
dt_timer_ms(timer)          # Get elapsed time in milliseconds
dt_timer_report(timer)      # Get human-readable timer report
```

#### Example

```kosh
let t = dt_timer_start()

# Do something...
t = dt_timer_lap(t)         # Lap 1
# Do more...
t = dt_timer_lap(t)         # Lap 2
t = dt_timer_stop(t)

print dt_timer_report(t)    # "Elapsed: 1234.5ms | Laps: 2"
```

---

## Human-Readable Duration

```kosh
dt_human(seconds)           # Format seconds as human-readable string
```

#### Examples

```kosh
print dt_human(0.5)         # "500ms"
print dt_human(30)          # "30s"
print dt_human(125)         # "2m 5s"
print dt_human(3661)        # "1h 1m"
```

---

## Sleep Helpers

```kosh
dt_sleep_ms(ms)             # Sleep for milliseconds
dt_sleep_s(s)               # Sleep for seconds
```

#### Examples

```kosh
print "Waiting..."
dt_sleep_ms(500)            # Wait 500ms
print "Done!"

dt_sleep_s(2)               # Wait 2 seconds
```

---

## Complete Example

```kosh
import "datetime"

# Timer demonstration
let timer = dt_timer_start()

# Simulate work
dt_sleep_ms(200)

timer = dt_timer_lap(timer)  # Lap 1
dt_sleep_ms(300)
timer = dt_timer_lap(timer)  # Lap 2

timer = dt_timer_stop(timer)
print dt_timer_report(timer)

# Current time
let now = dt_now()
print "Current time (UTC): " + dt_format_full(now)
print "Since epoch: " + dt_human(now)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |