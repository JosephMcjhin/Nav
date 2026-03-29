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

sim_state = 0 # 0: Point 1 (Bottom Left), 1: Point 2 (Bottom Right), 2: Moving

def poll_status():
    """Poll the web_app for calibration status to automatically advance simulation states."""
    global sim_state
    while True:
        try:
            req = urllib.request.Request(STATUS_URL)
            with urllib.request.urlopen(req, timeout=1) as response:
                data = json.loads(response.read().decode())
                if data.get("is_calibrated"):
                    sim_state = 2
                elif data.get("points", 0) >= 1:
                    sim_state = 1
                else:
                    sim_state = 0
        except Exception:
            pass
        time.sleep(1.0)


def generate_random_env():
    anchors = [
        (random.uniform(0, 10), random.uniform(0, 10)),
        (random.uniform(10, 20), random.uniform(0, 10)),
        (random.uniform(5, 15), random.uniform(10, 20))
    ]
    print(f"[Sim] Generated 3 Random Anchors in UWB space: {anchors}")

    scale = random.uniform(80.0, 120.0) 
    theta = random.uniform(-math.pi, math.pi)
    tx = random.uniform(-500, 500)
    ty = random.uniform(-500, 500)

    print(f"[Sim] Real Transform Matrix => Scale: {scale:.2f}, Theta: {math.degrees(theta):.2f} deg, Translate: ({tx:.1f}, {ty:.1f})")

    def uwb_to_ue(uwb_x, uwb_y):
        rx = uwb_x * math.cos(theta) - uwb_y * math.sin(theta)
        ry = uwb_x * math.sin(theta) + uwb_y * math.cos(theta)
        return (rx * scale + tx, ry * scale + ty)

    return uwb_to_ue

def start_simulation():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[Sim] Starting UWB Tag simulation. Sending to UDP {UDP_TARGET_IP}:{UDP_TARGET_PORT}")
    
    uwb_to_ue_func = generate_random_env()

    # Start the polling thread
    threading.Thread(target=poll_status, daemon=True).start()

    start_time = time.time()
    speed_mps = 3
    update_rate_hz = 10

    try:
        while True:
            elapsed = time.time() - start_time
            
            if sim_state == 0:
                # Bottom-Left (Point 1)
                tag_uwb_x = 0.0
                tag_uwb_y = 0.0
            elif sim_state == 1:
                # Bottom-Right (Point 2)
                tag_uwb_x = 10.0
                tag_uwb_y = 0.0
            else:
                # Moving back and forth (X goes from 0.0 to 10.0)
                tag_uwb_x = 5.0 + 5.0 * math.cos(elapsed * speed_mps / 3.0)
                tag_uwb_y = 0.0
            
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
            
            ue_target = uwb_to_ue_func(tag_uwb_x, tag_uwb_y)
            
            if int(elapsed * update_rate_hz) % (update_rate_hz) == 0:
                state_str = ["P1 (Bottom-Left)", "P2 (Bottom-Right)", "Moving"][sim_state]
                print(f"[Sim] [{state_str}] UWB({tag_uwb_x:.2f}, {tag_uwb_y:.2f}) -> Should move to UE({ue_target[0]:.0f}, {ue_target[1]:.0f})")
            
            time.sleep(1.0 / update_rate_hz)
            
    except KeyboardInterrupt:
        print("\n[Sim] Simulation stopped.")
        sock.close()

if __name__ == "__main__":
    print("==========================================================")
    print(" UWB 2-Point Calibration Simulation Test")
    print("==========================================================")
    print("1. Start the web_app.py server.")
    print("2. Run the Unreal Engine project.")
    print("3. Use the UE Calibration UI:")
    print("   - The Tag is currently holding at 'Bottom-Left'.")
    print("   - Move Character to the FIRST spot and click 'Capture 1'.")
    print("   - The Tag will automatically jump to 'Bottom-Right'.")
    print("   - Move Character to the SECOND spot and click 'Capture 2'.")
    print("   - Click 'Solve'. The Tag will automatically start moving!")
    print("==========================================================\n")
    start_simulation()
