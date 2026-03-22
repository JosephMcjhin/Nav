"""
test_beacon.py – Simulates UBeacon Tools sending periodic webhook data.

UBeacon Tools webhook payload format:
  {
    "uuid": "...",
    "major": 1,
    "minor": 1,
    "rssi": -65,         # signal strength (always present)
    "distance": 2.3,     # estimated distance in metres (sometimes present)
    "tx_power": -59,
    "mac": "AA:BB:CC:DD:EE:FF"
  }

Run with:
    python test_beacon.py [--url http://localhost:8090] [--mode walk|rssi]
"""

import argparse
import math
import time

import requests

SERVER_URL = "http://127.0.0.1:8090"


def post(path: str, payload: dict):
    try:
        r = requests.post(f"{SERVER_URL}{path}", json=payload, timeout=3)
        return r.json()
    except Exception as e:
        return {"error": str(e)}


def simulate_walk(steps: int = 20, delay: float = 0.5):
    """Simulate walking toward the beacon (decreasing distance)."""
    print("=== Simulating walk toward beacon (distance 5m → 0.5m) ===")
    # Reset state
    resp = post("/api/beacon/reset", {})
    print(f"Reset: {resp}")
    time.sleep(0.5)

    start_dist = 5.0
    end_dist = 0.5
    for i in range(steps + 1):
        t = i / steps
        distance = start_dist + (end_dist - start_dist) * t
        payload = {
            "uuid": "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0",
            "major": 1, "minor": 1,
            "rssi": -50 - int(t * 30),  # RSSI degrades as "phone moves away"
            "distance": distance,
            "tx_power": -59,
        }
        result = post("/api/beacon", payload)
        print(f"  dist={distance:.2f}m  → server: {result}")
        time.sleep(delay)

    print("=== Walk complete ===\n")


def simulate_rssi_only(steps: int = 15, delay: float = 0.8):
    """Simulate UBeacon Tools sending only RSSI (no distance field)."""
    print("=== Simulating RSSI-only updates (approaching beacon) ===")
    post("/api/beacon/reset", {})
    time.sleep(0.5)

    for i in range(steps + 1):
        t = i / steps
        rssi = -80 + int(t * 35)  # -80 dBm (far) → -45 dBm (close)
        payload = {
            "uuid": "E2C56DB5-DFFB-48D2-B060-D0F5A71096E0",
            "major": 1, "minor": 1,
            "rssi": rssi,
            "tx_power": -59,
        }
        result = post("/api/beacon", payload)
        print(f"  rssi={rssi} dBm → server: {result}")
        time.sleep(delay)

    print("=== RSSI simulation complete ===\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8090", help="Server URL")
    parser.add_argument("--mode", choices=["walk", "rssi", "both"], default="both")
    args = parser.parse_args()
    SERVER_URL = args.url.rstrip("/")

    if args.mode in ("walk", "both"):
        simulate_walk()
    if args.mode in ("rssi", "both"):
        simulate_rssi_only()
