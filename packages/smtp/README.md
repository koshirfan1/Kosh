# Kosh SMTP Package — v1.0.0

**Package**: `smtp`  
**Installation**: `kh install smtp`

## Overview

The `smtp` package provides email sending utilities for Kosh. Currently simulates email sending via console output; a future version will use a native socket-based SMTP client.

## Installation

```bash
kh install smtp
```

```kosh
import "smtp"
```

---

## Functions

```kosh
smtp_config(host, port, user, pass)  # Create SMTP configuration
send_email(cfg, to, subject, body)   # Send an email → bool
```

#### Example

```kosh
let cfg = smtp_config("smtp.gmail.com", 587, "user@gmail.com", "password")
send_email(cfg, "recipient@example.com", "Hello", "This is a test email")
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release (simulated) |
