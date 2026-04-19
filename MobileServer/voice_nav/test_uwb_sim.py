import socket
import json
import time
import math
import threading
import urllib.request

UDP_TARGET_IP   = "127.0.0.1"
UDP_TARGET_PORT = 9003
STATUS_URL      = "http://127.0.0.1:8090/api/calibrate/status"
ROTATION_URL    = "http://127.0.0.1:8090/api/set_rotation"

# 0: Point 1, 1: Point 2, 2: Point 3, 3: Moving (post-calibration)
sim_state = 0

def poll_status():
    """Poll the web_app for calibration status to automatically advance states."""
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


def send_rotation(yaw_deg: float):
    """HTTP POST yaw to /api/set_rotation (fire-and-forget, silent on error)."""
    try:
        rot_payload = json.dumps({"yaw": yaw_deg}).encode("utf-8")
        req = urllib.request.Request(
            ROTATION_URL,
            data=rot_payload,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        urllib.request.urlopen(req, timeout=0.5)
    except Exception:
        pass


def start_simulation():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[Sim] Starting UWB Tag simulation → UDP {UDP_TARGET_IP}:{UDP_TARGET_PORT}")

    # ── Calibration hold positions (UWB space) ──────────────────────────────
    CALIB_POINTS = [
        (0.0, 0.0),   # Point 1
        (8.0, 0.0),   # Point 2
        (0.0, 8.0),   # Point 3
    ]

    # ── Waypoints for post-calibration movement (UWB space, looping path) ───
    WAYPOINTS = [
        (1.0, 1.0),
        (7.0, 1.0),
        (7.0, 7.0),
        (1.0, 7.0),
    ]

    # ── Tunable movement parameters ─────────────────────────────────────────
    WALK_SPEED_MPS   = 1.0   # meters / second while walking
    ROTATE_SPEED_DPS = 90.0  # degrees / second while rotating
    UPDATE_HZ        = 10    # UDP send rate

    # ── State machine for post-calibration ──────────────────────────────────
    # Phase: "rotate" → rotate to face next waypoint
    #        "walk"   → move straight toward next waypoint
    wp_idx     = 0       # current destination waypoint
    phase      = "rotate"
    pos_x      = WAYPOINTS[-1][0]   # start near last waypoint so we rotate first
    pos_y      = WAYPOINTS[-1][1]
    current_yaw = 0.0   # degrees (UWB-space; 0=East)

    threading.Thread(target=poll_status, daemon=True).start()

    last_log_sec = -1

    try:
        while True:
            dt = 1.0 / UPDATE_HZ
            state = sim_state

            if state < 3:
                # ── Hold at calibration point ──
                tag_uwb_x, tag_uwb_y = CALIB_POINTS[state]
                yaw_to_send = 0.0
            else:
                # ── Post-calibration: rotate → walk → rotate → walk … ──
                wx, wy = WAYPOINTS[wp_idx]
                dx, dy = wx - pos_x, wy - pos_y
                dist   = math.hypot(dx, dy)

                # Angle toward target (degrees, 0=East, CCW+)
                target_yaw = math.degrees(math.atan2(dy, dx))

                if phase == "rotate":
                    # Angular error (shortest path)
                    diff = (target_yaw - current_yaw + 540) % 360 - 180  # [-180, 180)
                    max_step = ROTATE_SPEED_DPS * dt

                    if abs(diff) <= max_step:
                        current_yaw = target_yaw   # snap to exact angle
                        phase = "walk"
                        print(f"[Sim] ▶ Rotate done → now WALKING to WP{wp_idx} ({wx:.1f},{wy:.1f})")
                    else:
                        current_yaw += math.copysign(max_step, diff)
                    current_yaw = (current_yaw + 360) % 360

                elif phase == "walk":
                    if dist <= WALK_SPEED_MPS * dt:
                        # Arrived at waypoint
                        pos_x, pos_y = wx, wy
                        wp_idx = (wp_idx + 1) % len(WAYPOINTS)
                        phase  = "rotate"
                        print(f"[Sim] ✓ Arrived at ({wx:.1f},{wy:.1f}) → ROTATING to WP{wp_idx}")
                    else:
                        # Step forward
                        step = WALK_SPEED_MPS * dt
                        pos_x += (dx / dist) * step
                        pos_y += (dy / dist) * step

                tag_uwb_x = pos_x
                tag_uwb_y = pos_y
                yaw_to_send = current_yaw

                # Only send rotation command during rotate phase (or just after settling)
                # Send every tick for smooth rotation display
                threading.Thread(
                    target=send_rotation, args=(yaw_to_send,), daemon=True
                ).start()

            # ── UDP position packet ──────────────────────────────────────────
            payload = {
                "name":       "Pos",
                "deviceName": "T1",
                "uid":        "sim-tag-001",
                "data":       {"pos": [tag_uwb_x, tag_uwb_y, 0.0]}
            }
            sock.sendto(json.dumps(payload).encode("utf-8"),
                        (UDP_TARGET_IP, UDP_TARGET_PORT))

            # ── Console log (once per second) ────────────────────────────────
            cur_sec = int(time.time())
            if cur_sec != last_log_sec:
                last_log_sec = cur_sec
                if state < 3:
                    labels = ["P1(0,0)", "P2(8,0)", "P3(0,8)"]
                    print(f"[Sim] [{labels[state]}] holding")
                else:
                    print(f"[Sim] [{phase.upper():6}] pos=({tag_uwb_x:.2f},{tag_uwb_y:.2f})"
                          f" yaw={yaw_to_send:.1f}° → WP{wp_idx}({WAYPOINTS[wp_idx][0]:.1f},{WAYPOINTS[wp_idx][1]:.1f})")

            time.sleep(dt)

    except KeyboardInterrupt:
        print("\n[Sim] Simulation stopped.")
        sock.close()


if __name__ == "__main__":
    print("==========================================================")
    print(" UWB 3-Point Calibration Simulation  (Waypoint Edition)")
    print("==========================================================")
    print("1. Start web_app.py server.")
    print("2. Run the Unreal Engine project and connect.")
    print("3. Calibration UI:")
    print("   - Tag holds at P1(0,0) → Capture 1")
    print("   - Tag holds at P2(8,0) → Capture 2")
    print("   - Tag holds at P3(0,8) → Capture 3")
    print("   - Click Solve → Tag walks a square: rotate then walk each side.")
    print("==========================================================\n")
    start_simulation()
