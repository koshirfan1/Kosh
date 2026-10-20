# Kosh DNS Package — v1.0.0

**Package**: `dns`  
**Installation**: `kh install dns`

## Overview

The `dns` package provides DNS lookup utilities using DNS-over-HTTPS (Google DNS). It resolves domain names to IP addresses.

## Installation

```bash
kh install dns
kh install requests
```

```kosh
import "dns"
```

---

## Functions

```kosh
dns_lookup(domain)      # Resolve domain to IP address → string or nil
```

#### Example

```kosh
let ip = dns_lookup("example.com")
if ip != nil:
    print "example.com resolves to " + ip
else:
    print "DNS lookup failed"
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
