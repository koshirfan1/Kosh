# Kosh SSH Client Package — v1.0.0

**Package**: `ssh_client`  
**Installation**: `kh install ssh_client`

## Overview

The `ssh_client` package provides SSH client capabilities for remote command execution.

## Installation

```bash
kh install ssh_client
```

```kosh
import "ssh_client"
```

---

## Functions

```kosh
ssh_client(host, user, pass)    # Create a new SSH client
ssh_exec(client, cmd)           # Execute a remote command → string
```

#### Example

```kosh
let client = ssh_client("server.example.com", "admin", "password")
let output = ssh_exec(client, "ls -la /var/log")
print output
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
