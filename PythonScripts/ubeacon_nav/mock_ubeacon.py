import socket
import json
import time
import math

UDP_TARGET_IP = "127.0.0.1"
UDP_TARGET_PORT = 9003

def push_mock_data():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[MockUBeacon] Starting simulation. Sending to {UDP_TARGET_IP}:{UDP_TARGET_PORT}")
    
    # Starting position in meters
    current_x = 0.0
    current_y = 0.0
    current_z = 0.0
    
    # Movement speed: ~1 meter per second
    speed_mps = 2
    update_rate_hz = 15
    step_size = speed_mps / update_rate_hz

    start_time = time.time()
    
    try:
        while True:
            elapsed = time.time() - start_time
            
            # Simulate walking back and forth along the X axis
            # distance = 5 meters each way. Uses a sine wave.
            distance = 3
            
            current_x = distance * math.sin(elapsed * speed_mps / distance)
            current_y = 0.0
            
            # Assemble payload matching exact Nooploop protocol
            payload = {
                "name": "Pos",
                "uid": "mock-sensor-001",
                "data": {
                    "mapId": 1,
                    "pos": [current_x, current_y, current_z],
                    "time": int(time.time() * 1000)
                }
            }
            
            # Send via UDP
            msg = json.dumps(payload).encode("utf-8")
            sock.sendto(msg, (UDP_TARGET_IP, UDP_TARGET_PORT))
            
            print(f"[MockUBeacon] Sent Pos: {current_x:.3f}, {current_y:.3f}")
            time.sleep(1.0 / update_rate_hz)
            
    except KeyboardInterrupt:
        print("\n[MockUBeacon] Simulation stopped by user.")
        sock.close()

if __name__ == "__main__":
    push_mock_data()
