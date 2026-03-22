import socket
import json
import time
import math

# UE5 TCP Server Config
UE5_HOST = "127.0.0.1"
UE5_PORT = 9001

# UBeacon UDP Listener Config
UDP_PORT = 9003

def send_to_ue5(action, delta):
    """Sends a JSON command to the UE5 TCP Server."""
    command = {
        "action": action,
        "delta": delta
    }
    try:
        with socket.create_connection((UE5_HOST, UE5_PORT), timeout=1.0) as sock:
            msg = json.dumps(command) + "\n"
            sock.sendall(msg.encode("utf-8"))
    except ConnectionRefusedError:
        pass # UE5 is probably not running
    except Exception as e:
        print(f"[UBeaconServer] TCP error connecting to UE5: {e}")

def run_ubeacon_server():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", UDP_PORT))
    
    print(f"[UBeaconServer] Listening for UBeacon JSON UDP pushes on port {UDP_PORT}...")
    
    last_pos = None

    while True:
        try:
            data, addr = sock.recvfrom(4096)
            payload = data.decode("utf-8").strip()
            if not payload:
                continue

            parsed = json.loads(payload)
            # Documentation states {"name": "Pos", "data": {"pos": [x, y, z]}}
            if parsed.get("name") == "Pos" and "data" in parsed:
                pos_array = parsed["data"].get("pos")
                if pos_array and len(pos_array) >= 2:
                    current_x = float(pos_array[0])
                    current_y = float(pos_array[1])
                    current_z = float(pos_array[2]) if len(pos_array) > 2 else 0.0
                    
                    if last_pos is not None:
                        dx = current_x - last_pos[0]
                        dy = current_y - last_pos[1]
                        dz = current_z - last_pos[2]
                        
                        # Only send if there is significant movement to prevent jitter spam
                        if abs(dx) > 0.001 or abs(dy) > 0.001 or abs(dz) > 0.001:
                            # Unreal uses centimeters, UBeacon uses meters
                            delta_cm = [dx * 100.0, dy * 100.0, dz * 100.0]
                            send_to_ue5("add_world_offset", delta_cm)
                            print(f"[UBeaconServer] Delta {delta_cm} cm sent to UE5.")
                            
                    last_pos = (current_x, current_y, current_z)

        except json.JSONDecodeError:
            print("[UBeaconServer] Received invalid JSON payload.")
        except Exception as e:
            print(f"[UBeaconServer] UDP Processing Error: {e}")

if __name__ == "__main__":
    run_ubeacon_server()
