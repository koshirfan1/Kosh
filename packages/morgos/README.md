# Kosh Morgos Package — v1.0.0

**Package**: `morgos`  
**Installation**: `kh install morgos`

## Overview

Morgos is a Morgan-style HTTP request logger for the webi web framework. It logs every request with color-coded status codes, elapsed time, and response size — as a swappable `after()` hook.

## Installation

```bash
kh install webi
kh install morgos
```

```kosh
from webi import webi
import "morgos"
```

---

## Usage

```kosh
let app = webi_app()
app = after(app, morgos)
```

Every request produces one line:
```
GET /users/42 200 12.48 ms - 1024
```

Status codes are color-coded:
- **Green** (2xx) — Success
- **Cyan** (3xx) — Redirect
- **Yellow** (4xx) — Client error
- **Red** (5xx) — Server error

---

## Complete Example

```kosh
from webi import webi
import "morgos"

let app = webi_app()
app = after(app, morgos)

fn index(req):
    return res_text("Hello, World!")

app = route(app, "GET", "/", index)
webi_run(app, 8080)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
