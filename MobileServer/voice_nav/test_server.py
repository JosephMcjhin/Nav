import socket
import json
import time
import math
import threading
import urllib.request
import sys

# To support WebSocket, make sure to run: uv pip install websocket-client
try:
    import websocket
except ImportError:
    print("Error: 'websocket-client' is missing.")
    print("Please run: uv pip install websocket-client")
    sys.exit(1)

UDP_TARGET_IP   = "127.0.0.1"
UDP_TARGET_PORT = 9003
STATUS_URL      = "http://127.0.0.1:8090/api/calibrate/status"
WS_URL          = "ws://127.0.0.1:8090/ws"

# 0: Point 1, 1: Point 2, 2: Point 3, 3: Moving (post-calibration)
sim_state = 0
ws_app = None

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


def on_ws_message(ws, message):
    """Handle incoming WebSocket messages from the backend."""
    try:
        data = json.loads(message)
        msg_type = data.get("type")
        if msg_type == "nav_prompt":
            print(f"\n[导航提示] {data.get('text')}\n> ", end="", flush=True)
        elif msg_type == "status":
            print(f"\n[系统状态] {data.get('text')}\n> ", end="", flush=True)
    except Exception as e:
        pass

def on_ws_error(ws, error):
    pass

def on_ws_close(ws, close_status_code, close_msg):
    print("\n[WebSocket] Disconnected from server.")

def on_ws_open(ws):
    print("\n[WebSocket] Connected to server for Navigation Requests/Prompts.")

def start_ws_client():
    global ws_app
    ws_app = websocket.WebSocketApp(WS_URL,
                                    on_open=on_ws_open,
                                    on_message=on_ws_message,
                                    on_error=on_ws_error,
                                    on_close=on_ws_close)
    ws_app.run_forever()

def command_input_thread():
    """Thread to handle user input for sending nav requests."""
    global ws_app
    time.sleep(2) # Give it a moment to connect
    while True:
        try:
            target = input("\n> 请输入想要导航的目标字符串 (如: 会议桌, 沙发): \n> ")
            if target.strip() and ws_app and ws_app.sock and ws_app.sock.connected:
                req = {
                    "type": "nav_request",
                    "target": target.strip()
                }
                ws_app.send(json.dumps(req))
        except EOFError:
            break
        except Exception as e:
            pass


def start_simulation():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[Sim] Starting Sensor simulation → UDP {UDP_TARGET_IP}:{UDP_TARGET_PORT}")

    # ── Calibration hold positions (UWB space) ──────────────────────────────
    # We pretend the UWB tags are physically at these locations during capture.
    CALIB_POINTS = [
        (0.0, 0.0),   # Point 1
        (8.0, 0.0),   # Point 2
        (0.0, 8.0),   # Point 3
    ]

    # ── Waypoints for post-calibration movement (UWB space, looping path) ───
    # We walk in a square: walk straight, turn 90 deg, walk, turn 90 deg.
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
    threading.Thread(target=start_ws_client, daemon=True).start()
    threading.Thread(target=command_input_thread, daemon=True).start()

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
                        print(f"\n[Sim] ▶ Rotate done → now WALKING to WP{wp_idx} ({wx:.1f},{wy:.1f})\n> ", end="")
                    else:
                        current_yaw += math.copysign(max_step, diff)
                    current_yaw = (current_yaw + 360) % 360

                elif phase == "walk":
                    if dist <= WALK_SPEED_MPS * dt:
                        # Arrived at waypoint
                        pos_x, pos_y = wx, wy
                        wp_idx = (wp_idx + 1) % len(WAYPOINTS)
                        phase  = "rotate"
                        print(f"\n[Sim] ✓ Arrived at ({wx:.1f},{wy:.1f}) → ROTATING to WP{wp_idx}\n> ", end="")
                    else:
                        # Step forward
                        step = WALK_SPEED_MPS * dt
                        pos_x += (dx / dist) * step
                        pos_y += (dy / dist) * step

                tag_uwb_x = pos_x
                tag_uwb_y = pos_y
                yaw_to_send = current_yaw

            # ── UDP UWB position packet ──────────────────────────────────────
            uwb_payload = {
                "name":       "Pos",
                "deviceName": "T1",
                "uid":        "sim-tag-001",
                "data":       {"pos": [tag_uwb_x, tag_uwb_y, 0.0]}
            }
            sock.sendto(json.dumps(uwb_payload).encode("utf-8"),
                        (UDP_TARGET_IP, UDP_TARGET_PORT))

            # ── UDP IMU rotation packet ──────────────────────────────────────
            imu_payload = {
                "id": "G01", 
                "ts": int(time.time() * 1000), 
                "freq": 50,
                # euler: [Yaw, Pitch, Roll] (using euler[0] as yaw)
                "euler": [yaw_to_send, 0.0, 0.0]
            }
            sock.sendto(json.dumps(imu_payload).encode("utf-8"),
                        (UDP_TARGET_IP, UDP_TARGET_PORT))

            # ── Console log (once per 5 seconds so it doesn't spam input) ────
            cur_sec = int(time.time())
            if cur_sec != last_log_sec and cur_sec % 5 == 0:
                last_log_sec = cur_sec
                if state < 3:
                    labels = ["P1(0,0)", "P2(8,0)", "P3(0,8)"]
                    # Silently update internally without breaking input too much
                else:
                    pass # Less spam

            time.sleep(dt)

    except KeyboardInterrupt:
        print("\n[Sim] Simulation stopped.")
        sock.close()


if __name__ == "__main__":
    print("==========================================================")
    print(" 综合校准与导航测试终端 (UWB + IMU + WebSocket)")
    print("==========================================================")
    print("1. 确保已启动 web_app.py")
    print("2. 运行 Unreal Engine 并自动连接")
    print("3. 校准阶段 (通过 UE 界面按钮触发):")
    print("   - UE 端移动到点1并点击校准 → 模拟器此时停在 UWB P1(0,0)")
    print("   - UE 端移动到点2并点击校准 → 模拟器此时停在 UWB P2(8,0)")
    print("   - UE 端移动到点3并点击校准 → 模拟器此时停在 UWB P3(0,8)")
    print("   - UE 端点击解算，校准完成。")
    print("4. 测试阶段:")
    print("   - 模拟器开始沿正方形路径[走直线->转90度->走直线]循环。")
    print("   - 你可以直接在此终端输入地点（例如：会议桌），回车发送导航请求。")
    print("   - 终端将实时打印来自后端的导航提示。")
    print("==========================================================\n")
    start_simulation()
