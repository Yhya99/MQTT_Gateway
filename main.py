# main.py
# Updated for ChaCha20-Poly1305 encryption with derived key

import json
import time
import random
import argparse
import threading
import hashlib
import struct
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Install paho-mqtt:  pip install paho-mqtt")
    exit(1)

try:
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
except ImportError:
    print("Install cryptography:  pip install cryptography")
    exit(1)

# ──────────────────────────────────────────────
#  Configuration
# ──────────────────────────────────────────────
DEFAULT_BROKER    = "broker.hivemq.com"
DEFAULT_PORT      = 1883
DEFAULT_DEVICE_ID = "device_01"
DEFAULT_NAME      = "Test ping device 01"
DEFAULT_TYPE      = "TestDevice"
DEFAULT_PSK       = "test123"          # default PSK (plain text)

# Topics (must match ESP32 gateway)
T_GATEWAY_RX      = "jrpc/gateway/rx"
T_GATEWAY_CONNECT = "jrpc/gateway/connect"

class SensorDevice:
    DISCONNECTED = "disconnected"
    CONNECTING   = "connecting"
    CONNECTED    = "connected"

    def __init__(self, device_id, name, device_type, broker, port, psk=None):
        self.device_id   = device_id
        self.name        = name
        self.device_type = device_type
        self.broker      = broker
        self.port        = port
        self.rpc_id      = 0
        self.pending_rpc = {}

        self.state       = self.DISCONNECTED
        self.my_topic    = f"jrpc/devices/{self.device_id}/rx"

        # Derive 32‑byte encryption key from PSK (string) using SHA‑256
        if psk is None:
            psk = DEFAULT_PSK
        self.enc_key = hashlib.sha256(psk.encode('utf-8')).digest()   # 32 bytes
        self.counter = 0                               # for nonce, incremented each message

        # MQTT client
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION1,
            client_id=f"{self.device_id}_{random.randint(0,0xFFFF):04x}",
            protocol=mqtt.MQTTv311
        )

        self.client.on_connect    = self._on_connect
        self.client.on_message    = self._on_message
        self.client.on_disconnect = self._on_disconnect

        # Event to wait for connect response
        self.connect_event = threading.Event()
        self.connect_ok = False

    # ──────────────────────────────────────────
    #  MQTT Callbacks
    # ──────────────────────────────────────────
    def _on_connect(self, client, userdata, flags, rc):
        if rc != 0:
            self._log(f"MQTT connect failed: rc={rc}")
            return
        self._log(f"MQTT connected to {self.broker}")
        client.subscribe(self.my_topic, qos=1)
        self._log(f"Subscribed to {self.my_topic}")
        # After MQTT connect, start authentication
        self.connect()

    def _on_disconnect(self, client, userdata, rc):
        self._log(f"MQTT disconnected (rc={rc})")
        self.state = self.DISCONNECTED

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
        except json.JSONDecodeError:
            return
        # Responses are encrypted; they contain device_id, nonce, ciphertext
        if "device_id" in payload and "nonce" in payload and "ciphertext" in payload:
            self._handle_encrypted(payload)
        else:
            # Ignore plain messages (should not happen)
            pass

    # ──────────────────────────────────────────
    #  Encryption helpers
    # ──────────────────────────────────────────
    def _build_nonce(self):
        """Build 12‑byte nonce: 4‑byte counter (big‑endian) + 8‑byte timestamp."""
        self.counter += 1
        ts = int(time.time())
        # Pack counter as 4 bytes big‑endian, timestamp as 8 bytes big‑endian
        return struct.pack('>I', self.counter) + struct.pack('>Q', ts)

    def _encrypt(self, plaintext):
        """Encrypt plaintext (bytes) using current nonce. AAD = device_id."""
        nonce = self._build_nonce()
        # Use device_id as AAD. The ESP32's chacha20_poly1305_decrypt does not
        # verify the Poly1305 tag, so the AAD value does not need to match on
        # the decrypt side — decryption succeeds regardless.
        aad = self.device_id.encode('utf-8')
        cipher = ChaCha20Poly1305(self.enc_key)
        ciphertext = cipher.encrypt(nonce, plaintext, aad)
        return nonce, ciphertext

    def _decrypt(self, nonce, ciphertext):
        """Decrypt ciphertext using given nonce. AAD = device_id."""
        # Same as _encrypt — device_id as AAD, tag not verified on ESP32 side.
        aad = self.device_id.encode('utf-8')
        cipher = ChaCha20Poly1305(self.enc_key)
        try:
            plain = cipher.decrypt(nonce, ciphertext, aad)
            return plain
        except Exception as e:
            self._log(f"Decryption failed: {e}")
            return None

    # ──────────────────────────────────────────
    #  Connect procedure
    # ──────────────────────────────────────────
    def connect(self):
        """Send encrypted connect request and wait for approval (may take time)."""
        if self.state != self.DISCONNECTED:
            return
        self.state = self.CONNECTING
        self._log("Sending encrypted connect request...")

        # Inner JSON (will be encrypted)
        inner = {
            "device_name": self.name,
            "device_type": self.device_type
        }
        plaintext = json.dumps(inner, separators=(',', ':')).encode('utf-8')

        nonce, ciphertext = self._encrypt(plaintext)

        outer = {
            "device_id": self.device_id,
            "nonce": nonce.hex(),
            "ciphertext": ciphertext.hex()
        }

        self.client.publish(T_GATEWAY_CONNECT, json.dumps(outer, separators=(',', ':')), qos=1)

        # Wait for response – we wait longer (120 seconds) because admin must act
        if self.connect_event.wait(120.0):
            if self.connect_ok:
                self.state = self.CONNECTED
                self._log("Connected and approved by gateway.")
            else:
                self.state = self.DISCONNECTED
                self._log("Connect rejected by gateway.")
        else:
            self.state = self.DISCONNECTED
            self._log("Connect timeout – no response from gateway.")

    def _handle_encrypted(self, payload):
        """Process an encrypted response from the gateway."""
        device_id = payload.get("device_id")
        if device_id != self.device_id:
            return  # not for us
        nonce_hex = payload.get("nonce")
        cipher_hex = payload.get("ciphertext")
        if not nonce_hex or not cipher_hex:
            return
        nonce = bytes.fromhex(nonce_hex)
        cipher = bytes.fromhex(cipher_hex)
        plain = self._decrypt(nonce, cipher)
        if plain is None:
            return
        try:
            msg = json.loads(plain.decode('utf-8'))
        except json.JSONDecodeError:
            return

        # Check if it's a connect response
        if msg.get("method") == "connect.response":
            status = msg.get("params", {}).get("status")
            self.connect_ok = (status == "approved")
            self.connect_event.set()
        elif "result" in msg:
            self._handle_result(msg)
        elif "error" in msg:
            self._handle_error(msg)

    # ──────────────────────────────────────────
    #  RPC handling (after connected)
    # ──────────────────────────────────────────
    def send_rpc(self, method, params):
        """Send an encrypted RPC to the gateway."""
        if self.state != self.CONNECTED:
            self._log("Not connected – cannot send RPC.")
            return

        rpc_id = self._next_id()
        inner = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params,
            "id": rpc_id
        }
        plaintext = json.dumps(inner, separators=(',', ':')).encode('utf-8')
        nonce, ciphertext = self._encrypt(plaintext)

        outer = {
            "device_id": self.device_id,
            "nonce": nonce.hex(),
            "ciphertext": ciphertext.hex()
        }

        self.pending_rpc[rpc_id] = {"method": method, "ts": time.time()}
        self.client.publish(T_GATEWAY_RX, json.dumps(outer, separators=(',', ':')), qos=1)

    def send_ping(self):
        """Ping the gateway."""
        self._log("  Pinging gateway...")
        self.send_rpc("ping", {})

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

    def _next_id(self):
        self.rpc_id += 1
        return self.rpc_id

    def _log(self, msg, quiet=False):
        if quiet:
            return
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] [{self.device_id}] {msg}")

    def stop(self):
        self.client.disconnect()
        self.client.loop_stop()
        self._log("Disconnected.")

# ──────────────────────────────────────────────
#  Interactive Mode
# ──────────────────────────────────────────────
def interactive_mode(dev: SensorDevice):
    """Manual command mode for testing."""
    dev.client.connect(dev.broker, dev.port, keepalive=60)
    dev.client.loop_start()

    # Wait for connection and authentication (may take a while)
    time.sleep(2)  # give time for MQTT connect
    if dev.state != dev.CONNECTED:
        print("Waiting for admin approval via dashboard... (timeout 120s)")
        # The connect() method already waits, so we just wait for it to finish
        # Actually connect() is called from _on_connect, so we just wait here.
        # We'll wait up to 120 seconds.
        start = time.time()
        while dev.state != dev.CONNECTED and time.time() - start < 120:
            time.sleep(1)
        if dev.state != dev.CONNECTED:
            print("Failed to connect/authenticate. Check PSK and gateway.")
            dev.stop()
            return

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
        description="Device Simulator with ChaCha20‑Poly1305 encryption",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
        Examples:
        python main.py --device-id device_01 \\
            --name "Test device" --type TestDevice --psk test123
        """
    )

    parser.add_argument("--device-id", default=DEFAULT_DEVICE_ID)
    parser.add_argument("--name", default=DEFAULT_NAME, help="Human-readable device name")
    parser.add_argument("--type", default=DEFAULT_TYPE, dest="device_type",
                        help="Device type: sensor, actuator, controller")
    parser.add_argument("--psk", help="Pre‑shared key (plain text, e.g., 'test123')")
    parser.add_argument("--broker", default=DEFAULT_BROKER)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)

    args = parser.parse_args()

    dev = SensorDevice(
        args.device_id, args.name, args.device_type,
        args.broker, args.port, args.psk
    )
    interactive_mode(dev)