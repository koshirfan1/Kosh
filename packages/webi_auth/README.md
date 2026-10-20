# Kosh Webi Auth Package — v1.0.0

**Package**: `webi_auth`  
**Installation**: `kh install webi_auth`

## Overview

The `webi_auth` package provides authentication middleware for the webi web framework. It includes middleware for requiring Authorization headers and API key validation.

## Installation

```bash
kh install webi
kh install webi_auth
```

```kosh
import "webi_auth"
```

---

## Functions

```kosh
mw_auth(req)                    # Middleware: require Authorization header → short-circuit if missing
mw_require_key(req, valid_key)  # Middleware: require specific Bearer API key
auth_create_token(user_id)      # Create a simple auth token for a user
```

#### Example

```kosh
from webi import webi
import "webi_auth"

let app = webi_app()

# Protect all routes with API key
app = use(app, mw_require_key, "my-secret-key-123")

fn secret_data(req):
    return res_json({"secret": "data"})

app = route(app, "GET", "/data", secret_data)
webi_run(app, 8080)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
