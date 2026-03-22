import socket
import json
import time
import math
import random
import threading

UDP_TARGET_IP = "127.0.0.1"
UDP_TARGET_PORT = 9003

# Simulate Anchors and Transform Matrix
# In a real situation, UWB system solves Tag pos based on distances to Anchors.
# Here we just generate a random transform matrix mapping UE (cm) to UWB (m), 
# randomize anchors for visualization, and simulate tag walking.

def generate_random_env():
    # Random anchors
    anchors = [
        (random.uniform(0, 10), random.uniform(0, 10)),
        (random.uniform(10, 20), random.uniform(0, 10)),
        (random.uniform(5, 15), random.uniform(10, 20))
    ]
    print(f"[Sim] Generated 3 Random Anchors in UWB space: {anchors}")

    # Transform: UE = S * R * UWB + T
    scale = random.uniform(80.0, 120.0) # approx 1m = 100cm
    theta = random.uniform(-math.pi, math.pi)
    tx = random.uniform(-500, 500)
    ty = random.uniform(-500, 500)

    print(f"[Sim] Real (hidden) Transform Matrix => Scale: {scale:.2f}, Rotation: {math.degrees(theta):.2f} deg, Translation: ({tx:.1f}, {ty:.1f})")

    def uwb_to_ue(uwb_x, uwb_y):
        rx = uwb_x * math.cos(theta) - uwb_y * math.sin(theta)
        ry = uwb_x * math.sin(theta) + uwb_y * math.cos(theta)
        return (rx * scale + tx, ry * scale + ty)

    return uwb_to_ue

def start_simulation():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[Sim] Starting UWB Tag simulation. Sending to UDP {UDP_TARGET_IP}:{UDP_TARGET_PORT}")
    
    uwb_to_ue_func = generate_random_env()

    start_time = time.time()
    speed_mps = 1.5
    update_rate_hz = 10

    try:
        while True:
            elapsed = time.time() - start_time
            
            # Tag moves back and forth in a single direction (X axis) for 3 meters
            # UWB space units are meters. 10.0m to 13.0m loop.
            tag_uwb_x = 10.0 + 1.5 + 1.5 * math.cos(elapsed * speed_mps / 3.0)
            tag_uwb_y = 10.0
            
            payload = {
                "name": "Pos",
                "uid": "sim-tag-001",
                "data": {
                    "mapId": 1,
                    "pos": [tag_uwb_x, tag_uwb_y, 0.0],
                    "time": int(time.time() * 1000)
                }
            }
            
            msg = json.dumps(payload).encode("utf-8")
            sock.sendto(msg, (UDP_TARGET_IP, UDP_TARGET_PORT))
            
            ue_target = uwb_to_ue_func(tag_uwb_x, tag_uwb_y)
            
            # Only print every 1 second to avoid spam
            if int(elapsed * update_rate_hz) % (update_rate_hz) == 0:
                print(f"[Sim] Tag at UWB({tag_uwb_x:.2f}, {tag_uwb_y:.2f}) -> Should move to UE({ue_target[0]:.0f}, {ue_target[1]:.0f})")
            
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
    print("3. When you are ready, use the UE Calibration UI:")
    print("   - Wait for the Tag to reach a desired spot (monitor logs).")
    print("   - Move Character to that spot and click 'Capture 1'.")
    print("   - Wait for Tag to move to another spot.")
    print("   - Move Character to that second spot and click 'Capture 2'.")
    print("   - Click 'Solve'. The Character should now follow the Tag!")
    print("==========================================================\n")
    start_simulation()
