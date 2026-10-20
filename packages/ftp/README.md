# Kosh FTP Package — v1.0.0

**Package**: `ftp`  
**Installation**: `kh install ftp`

## Overview

The `ftp` package provides FTP client capabilities for file transfers. Supports upload and download operations.

## Installation

```bash
kh install ftp
```

```kosh
import "ftp"
```

---

## Functions

```kosh
ftp_client(host, user, pass)        # Create a new FTP client
ftp_upload(client, local, remote)   # Upload a file → bool
ftp_download(client, remote, local) # Download a file → bool
```

#### Example

```kosh
let client = ftp_client("ftp.example.com", "user", "password")
ftp_upload(client, "local.txt", "/remote/backup.txt")
ftp_download(client, "/remote/data.txt", "local_copy.txt")
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
