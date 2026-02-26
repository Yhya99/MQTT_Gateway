# MQTT_Gateway
### An implementation for adding authentications and authorization for the MQTT messages without depend on the broker side


## Overview

This project is an IoT gateway system with two components:

1. **ESP32 Gateway** (`MQTT_Gateway.ino`) — Runs on an ESP32 microcontroller. Hosts a web dashboard, manages device connections over MQTT, and dispatches JSON-RPC calls using the Mongoose networking library.
2. **Sensor Simulator** — A Python script that simulates IoT sensor devices for testing. Connects to the same MQTT broker and goes through the full authentication flow.

**Architecture:**

```
┌──────────────┐       MQTT (broker.hivemq.com)       ┌──────────────────┐
│  ESP32       │◄─────────────────────────────────────►│  Sensor Device   │
│  Gateway     │   jrpc/connect/request                │  (Python or real │
│              │   jrpc/gateway/rx                      │   ESP32 sensor)  │
│  Port 80:    │   jrpc/devices/{id}/rx                └──────────────────┘
│  Dashboard   │
│  + WebSocket │◄──── Browser (real-time dashboard)
└──────────────┘
```
