import json
import math
import socket
import sys
import threading
import time
import urllib.request

# To support WebSocket, make sure to run: uv pip install websocket-client
try:
    import websocket
except ImportError:
    print("Error: 'websocket-client' is missing.")
    print("Please run: uv pip install websocket-client")
    sys.exit(1)

UDP_TARGET_IP = "127.0.0.1"
UDP_TARGET_PORT = 9003
STATUS_URL = "http://127.0.0.1:8090/api/calibrate/status"
WS_URL = "ws://127.0.0.1:8090/ws"

# 0: Point 1, 1: Point 2, 2: Point 3, 3: Moving (post-calibration)
sim_state = 0
ws_app = None


def poll_status():
    """Poll the backend status and advance the simulation stage."""
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
    except Exception:
        pass


def on_ws_error(ws, error):
    pass


def on_ws_close(ws, close_status_code, close_msg):
    print("\n[WebSocket] Disconnected from server.")


def on_ws_open(ws):
    print("\n[WebSocket] Connected to server for Navigation Requests/Prompts.")


def start_ws_client():
    global ws_app
    ws_app = websocket.WebSocketApp(
        WS_URL,
        on_open=on_ws_open,
        on_message=on_ws_message,
        on_error=on_ws_error,
        on_close=on_ws_close,
    )
    ws_app.run_forever()


def command_input_thread():
    """Thread to handle user input for sending nav requests."""
    global ws_app
    time.sleep(2)
    while True:
        try:
            target = input("\n> 请输入想要导航的目标字符串（如：会议桌、沙发）:\n> ")
            if target.strip() and ws_app and ws_app.sock and ws_app.sock.connected:
                req = {
                    "type": "nav_request",
                    "target": target.strip(),
                }
                ws_app.send(json.dumps(req))
        except EOFError:
            break
        except Exception:
            pass


def start_simulation():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"[Sim] Starting Sensor simulation -> UDP {UDP_TARGET_IP}:{UDP_TARGET_PORT}")

    # Calibration hold positions in UWB space.
    CALIB_POINTS = [
        (0.0, 0.0),
        (5.0, 0.0),
        (0.0, 5.0),
    ]

    # Post-calibration loop:
    # 1. Walk  (0,0) -> (5,0), then rotate CCW 135 deg
    # 2. Walk  (5,0) -> (0,5), then rotate CCW 135 deg
    # 3. Walk  (0,5) -> (0,0), then rotate CCW  90 deg
    LOOP_SEGMENTS = [
        {"target": (5.0, 0.0), "turn_ccw_deg": 135.0},
        {"target": (0.0, 5.0), "turn_ccw_deg": 135.0},
        {"target": (0.0, 0.0), "turn_ccw_deg": 90.0},
    ]

    WALK_SPEED_MPS = 1.0
    ROTATE_SPEED_DPS = 90.0
    UPDATE_HZ = 10

    segment_idx = 0
    phase = "walk"
    pos_x = 0.0
    pos_y = 0.0
    current_yaw = 0.0
    rotate_remaining_deg = 0.0

    threading.Thread(target=poll_status, daemon=True).start()
    threading.Thread(target=start_ws_client, daemon=True).start()
    threading.Thread(target=command_input_thread, daemon=True).start()

    last_log_sec = -1

    try:
        while True:
            dt = 1.0 / UPDATE_HZ
            state = sim_state

            if state < 3:
                tag_uwb_x, tag_uwb_y = CALIB_POINTS[state]
                yaw_to_send = 0.0
            else:
                segment = LOOP_SEGMENTS[segment_idx]
                wx, wy = segment["target"]
                dx = wx - pos_x
                dy = wy - pos_y
                dist = math.hypot(dx, dy)

                if phase == "walk":
                    if dist <= WALK_SPEED_MPS * dt:
                        pos_x, pos_y = wx, wy
                        rotate_remaining_deg = segment["turn_ccw_deg"]
                        phase = "rotate"
                        print(
                            f"\n[Sim] Arrived at ({wx:.1f}, {wy:.1f}) -> "
                            f"ROTATING CCW {rotate_remaining_deg:.0f} deg\n> ",
                            end="",
                        )
                    else:
                        step = WALK_SPEED_MPS * dt
                        pos_x += (dx / dist) * step
                        pos_y += (dy / dist) * step

                elif phase == "rotate":
                    step_deg = min(rotate_remaining_deg, ROTATE_SPEED_DPS * dt)
                    current_yaw = (current_yaw + step_deg) % 360.0
                    rotate_remaining_deg -= step_deg

                    if rotate_remaining_deg <= 1e-6:
                        segment_idx = (segment_idx + 1) % len(LOOP_SEGMENTS)
                        next_target = LOOP_SEGMENTS[segment_idx]["target"]
                        phase = "walk"
                        print(
                            f"\n[Sim] Rotate done -> now WALKING to "
                            f"({next_target[0]:.1f}, {next_target[1]:.1f}) "
                            f"with yaw {current_yaw:.1f}\n> ",
                            end="",
                        )

                tag_uwb_x = pos_x
                tag_uwb_y = pos_y
                yaw_to_send = (-current_yaw) % 360.0

            should_send_uwb = (state < 3) or (phase == "walk")
            if should_send_uwb:
                uwb_payload = {
                    "name": "Pos",
                    "deviceName": "T1",
                    "uid": "sim-tag-001",
                    "data": {"pos": [tag_uwb_x, tag_uwb_y, 0.0]},
                }
                sock.sendto(
                    json.dumps(uwb_payload).encode("utf-8"),
                    (UDP_TARGET_IP, UDP_TARGET_PORT),
                )

            imu_payload = {
                "id": "G01",
                "ts": int(time.time() * 1000),
                "freq": 50,
                "euler": [yaw_to_send, 0.0, 0.0],
            }
            sock.sendto(
                json.dumps(imu_payload).encode("utf-8"),
                (UDP_TARGET_IP, UDP_TARGET_PORT),
            )

            cur_sec = int(time.time())
            if cur_sec != last_log_sec and cur_sec % 5 == 0:
                last_log_sec = cur_sec
                if state < 3:
                    labels = ["P1(0,0)", "P2(5,0)", "P3(0,5)"]
                    _ = labels[state]

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
    print("   - 点1 对应 UWB P1(0,0)，朝向 0 度")
    print("   - 点2 对应 UWB P2(5,0)，朝向 0 度")
    print("   - 点3 对应 UWB P3(0,5)，朝向 0 度")
    print("   - UE 端点击解算后，进入循环运动测试")
    print("4. 测试阶段:")
    print("   - (0,0) -> (5,0) -> 左转 135 度")
    print("   - (5,0) -> (0,5) -> 左转 135 度")
    print("   - (0,5) -> (0,0) -> 左转 90 度")
    print("   - 按以上序列循环")
    print("==========================================================\n")
    start_simulation()
