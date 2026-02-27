# this python code to test the arduino code

import json
import time
import random
import argparse
import threading
import os
from datetime import datetime



try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Install paho-mqtt:  pip install paho-mqtt")
    exit(1)



# ──────────────────────────────────────────────
#  Configuration
# ──────────────────────────────────────────────
DEFAULT_BROKER    = "broker.hivemq.com"
DEFAULT_PORT      = 1883
DEFAULT_DEVICE_ID = "device_01"
DEFAULT_NAME      = "Test ping device 01"
DEFAULT_TYPE      = "TestDevice"

# Topics (must match ESP32 gateway)
T_GATEWAY_RX      = "jrpc/gateway/rx"

class SensorDevice:

    # ── Connection states ──
    DISCONNECTED = "disconnected"
    CONNECTED   = "connected"

    def __init__(self,device_id,name,device_type,broker,port):
        self.device_id   = device_id
        self.name        = name
        self.device_type = device_type
        self.broker      = broker
        self.port        = port
        self.rpc_id      = 0
        self.pending_rpc = {}

        self.state       = self.DISCONNECTED
        self.my_topic = f"jrpc/devices/{self.device_id}/rx"

        # MQTT client
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION1,
            client_id=f"{self.device_id}_{random.randint(0,0xFFFF):04x}",
            protocol=mqtt.MQTTv311
        )

        self.client.on_connect    = self._on_connect
        self.client.on_message    = self._on_message
        self.client.on_disconnect = self._on_disconnect

    # ──────────────────────────────────────────
    #  MQTT Callbacks
    # ──────────────────────────────────────────
    def _on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            self._log(f"MQTT connect failed: rc={rc}")
            return

        self._log(f"MQTT connected to {self.broker}")

        # Subscribe to our response topic + broadcast
        client.subscribe(self.my_topic, qos=1)
        self._log(f"Subscribed to {self.my_topic}")

        self.state = self.CONNECTED

    def _on_disconnect(self, client, userdata, rc):
        self._log(f"MQTT disconnected (rc={rc})")
        self.state = self.DISCONNECTED
        pass

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
        except json.JSONDecodeError:
            return
        topic = msg.topic
        method = payload.get("method", "")
        # implement for message response for methods
        # ── RPC Responses (result / error) ──
        if "result" in payload:
            self._handle_result(payload)
        elif "error" in payload:
            self._handle_error(payload)
        pass

    def _log(self, msg, quiet=False):
        if quiet:
            return  # Suppress noisy heartbeat logs
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] [{self.device_id}] {msg}")
        pass
    
    def stop(self):
        self.running = False
        self.client.disconnect()
        self.client.loop_stop()
        self._log("Disconnected.")
        pass
    # ──────────────────────────────────────────
    #  Response handlers
    # ──────────────────────────────────────────
    def _handle_result(self, payload):
        rpc_id = payload.get("id", -1)
        result = payload.get("result", {})
        info   = self.pending_rpc.pop(rpc_id, None)
        method = info["method"] if info else "?"
        rtt    = (time.time() - info["ts"]) * 1000 if info else 0

        if method == "ping":
            uptime = result.get("uptime_ms", 0) / 1000
            self._log(f"  Pong! Gateway uptime: {uptime:.0f}s (RTT {rtt:.0f}ms)")
        else:
            self._log(f"  [RESULT] {method}: {json.dumps(result)}")

    def _handle_error(self, payload):
        rpc_id = payload.get("id", -1)
        error  = payload.get("error", {})
        info   = self.pending_rpc.pop(rpc_id, None)
        method = info["method"] if info else "?"
        code   = error.get("code", "?")
        msg    = error.get("message", "?")
        self._log(f"  ERROR [{method}] code={code}: {msg}")
        pass

    def _next_id(self):
        self.rpc_id += 1
        return self.rpc_id
        
    def send_rpc(self,method,params):
        ts = int(time.time())
        rpc_id = self._next_id()
        params["device_id"] = self.device_id

        message = {
            "v": 1,
            "device_id": self.device_id,
            "nonce": 0,
            "method" : method,
            "params" : params,
            "id"     : rpc_id
        }

        self.pending_rpc[rpc_id] = {"method": method, "ts": time.time()}
        self.client.publish(T_GATEWAY_RX, json.dumps(message, separators=(',', ':')), qos=1)

        pass
    def send_ping(self):
        """Ping the gateway."""
        self._log("  Pinging gateway...")
        self.send_rpc("ping", {})
        pass
    




# ──────────────────────────────────────────────
#  Interactive Mode
# ──────────────────────────────────────────────
def interactive_mode(dev: SensorDevice):
    """Manual command mode for testing."""
    dev.client.connect(dev.broker, dev.port, keepalive=60)
    dev.client.loop_start()

    time.sleep(3)  # Wait for connect

    print("\n+--- Commands ---------------------+")
    print("|  ping     -- Ping gateway         |")
    print("|  quit     -- Exit                 |")
    print("+----------------------------------+\n")

    try:
        while True:
            cmd = input(f"[{dev.device_id}|{dev.state}] > ").strip().lower()

            if cmd == "ping":
                dev.send_ping()
            elif cmd in ("quit", "exit", "q"):
                break
            elif cmd == "":
                continue
            else:
                print(f"  Unknown: '{cmd}'")
            time.sleep(0.5)
    except (KeyboardInterrupt, EOFError):
        pass
    dev.stop()

# ──────────────────────────────────────────────
#  CLI Entry Point
# ──────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Device Simulator",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
        Examples:
        python main.py --device-id device_01 \\
        --name "Test device" --type TestDevice
        """
    ) 

    parser.add_argument("--device-id", default=DEFAULT_DEVICE_ID)
    parser.add_argument("--name", default=DEFAULT_NAME, help="Human-readable device name")
    parser.add_argument("--type", default=DEFAULT_TYPE, dest="device_type",
                        help="Device type: sensor, actuator, controller")
    parser.add_argument("--broker", default=DEFAULT_BROKER)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)

    args = parser.parse_args()
    dev = SensorDevice(
            args.device_id, args.name, args.device_type,
            args.broker, args.port)
    interactive_mode(dev)
