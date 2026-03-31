import socket
import json
import time
import math
import random
import threading
import urllib.request

UDP_TARGET_IP = "127.0.0.1"
UDP_TARGET_PORT = 9003
STATUS_URL = "http://127.0.0.1:8090/api/calibrate/status"

# 0: Point 1, 1: Point 2, 2: Point 3, 3: Circling
sim_state = 0

def poll_status():
    """Poll the web_app for calibration status to automatically advance simulation states."""
    global sim_state
    while True:
        try:
            req = urllib.request.Request(STATUS_URL)
            with urllib.request.urlopen(req, timeout=1) as response:
                data = json.loads(response.read().decode())
                if data.get("is_calibrated"):
                    sim_state = 3
                elif data.get("points", 0) >= 2:
                    sim_state = 2
                elif data.get("points", 0) >= 1:
                    sim_state = 1
                else:
                    sim_state = 0
        except Exception:
            pass
        time.sleep(1.0)


def start_simulation():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[Sim] Starting UWB Tag simulation. Sending to UDP {UDP_TARGET_IP}:{UDP_TARGET_PORT}")

    # Calibration hold positions (UWB space)
    CALIB_POINTS = [
        (0.0, 0.0),   # Point 1: origin
        (8.0, 0.0),   # Point 2: X axis
        (0.0, 8.0),   # Point 3: Y axis
    ]

    # Circle parameters
    CIRCLE_CENTER_X = 6.0
    CIRCLE_CENTER_Y = 6.0
    CIRCLE_RADIUS   = 3.0   # meters in UWB space
    CIRCLE_SPEED    = 0.5   # radians per second

    # Start the polling thread
    threading.Thread(target=poll_status, daemon=True).start()

    circle_start_time = None
    update_rate_hz = 10

    try:
        while True:
            state = sim_state

            if state < 3:
                # Hold at the calibration position for the current point
                tag_uwb_x, tag_uwb_y = CALIB_POINTS[state]
            else:
                # Circling around (6, 6)
                if circle_start_time is None:
                    circle_start_time = time.time()
                t = time.time() - circle_start_time
                angle = t * CIRCLE_SPEED
                tag_uwb_x = CIRCLE_CENTER_X + CIRCLE_RADIUS * math.cos(angle)
                tag_uwb_y = CIRCLE_CENTER_Y + CIRCLE_RADIUS * math.sin(angle)

            payload = {
                "name": "Pos",
                "deviceName": "T1",
                "uid": "sim-tag-001",
                "data": {
                    "pos": [tag_uwb_x, tag_uwb_y, 0.0]
                }
            }

            msg = json.dumps(payload).encode("utf-8")
            sock.sendto(msg, (UDP_TARGET_IP, UDP_TARGET_PORT))

            state_labels = ["P1 (0,0)", "P2 (8,0)", "P3 (0,8)", "Circling (6,6)"]
            now_ms = int(time.time() * update_rate_hz)
            if now_ms % update_rate_hz == 0:
                print(f"[Sim] [{state_labels[state]}] UWB({tag_uwb_x:.2f}, {tag_uwb_y:.2f})")

            time.sleep(1.0 / update_rate_hz)

    except KeyboardInterrupt:
        print("\n[Sim] Simulation stopped.")
        sock.close()


if __name__ == "__main__":
    print("==========================================================")
    print(" UWB 3-Point Calibration Simulation Test")
    print("==========================================================")
    print("1. Start the web_app.py server.")
    print("2. Run the Unreal Engine project.")
    print("3. Use the UE Calibration UI:")
    print("   - Tag holds at Point 1 (UWB 0,0). Move Character, click 'Capture 1'.")
    print("   - Tag jumps to Point 2 (UWB 8,0). Move Character, click 'Capture 2'.")
    print("   - Tag jumps to Point 3 (UWB 0,8). Move Character, click 'Capture 3'.")
    print("   - Click 'Solve'. Tag will circle around (6,6) in UWB space!")
    print("==========================================================\n")
    start_simulation()
