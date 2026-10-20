# Kosh Webi Socket Package — v1.0.0

**Package**: `webi_socket`  
**Installation**: `kh install webi_socket`

## Overview

The `webi_socket` package provides WebSocket support for the webi web framework. It enables real-time, bidirectional communication between clients and the server.

## Installation

```bash
kh install webi
kh install webi_socket
```

```kosh
import "webi_socket"
```

---

## Functions

```kosh
ws_route(app, path, handler)    # Register a WebSocket route
ws_send(ws_id, message)         # Send message to a client → bool
ws_broadcast(message)           # Broadcast message to all clients → bool
```

#### Example

```kosh
from webi import webi
import "webi_socket"

let app = webi_app()

fn chat_handler(req, ws):
    ws_send(ws, "Welcome to chat!")

app = ws_route(app, "/chat", chat_handler)
webi_run(app, 8080)
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
