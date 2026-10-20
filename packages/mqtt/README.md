# Kosh MQTT Package — v1.0.0

**Package**: `mqtt`  
**Installation**: `kh install mqtt`

## Overview

The `mqtt` package provides MQTT protocol support for IoT messaging. It enables publish/subscribe communication patterns for lightweight message exchange.

## Installation

```bash
kh install mqtt
```

```kosh
import "mqtt"
```

---

## Functions

```kosh
mqtt_client(host, port)         # Create a new MQTT client
mqtt_publish(client, topic, message)  # Publish a message to a topic → bool
mqtt_subscribe(client, topic)   # Subscribe to a topic → client
```

#### Example

```kosh
let client = mqtt_client("broker.hivemq.com", 1883)
client = mqtt_subscribe(client, "sensors/temperature")
mqtt_publish(client, "sensors/temperature", "23.5")
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
