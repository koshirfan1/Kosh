# Kosh Swagger Package — v1.0.0

**Package**: `swagger`  
**Installation**: `kh install swagger`

## Overview

The `swagger` package provides an API documentation generator for the webi framework. It generates a Swagger-style HTML documentation page listing all registered routes.

## Installation

```bash
kh install webi
kh install swagger
```

```kosh
import "swagger"
```

---

## Functions

```kosh
swagger_ui(app, path)       # Add Swagger UI endpoint to app → updated app
```

#### Example

```kosh
from webi import webi
import "swagger"

let app = webi_app()

fn hello(req):
    return res_json({"message": "Hello"})

app = route(app, "GET", "/hello", hello)
app = swagger_ui(app, "/docs")

webi_run(app, 8080)
# Visit http://localhost:8080/docs to see API documentation
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
