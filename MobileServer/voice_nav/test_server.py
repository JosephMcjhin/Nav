import json
import math
import random
import socket
import threading
import time
import urllib.request

UDP_TARGET_IP = "127.0.0.1"
UDP_TARGET_PORT = 9003
STATUS_URL = "http://127.0.0.1:8090/api/calibrate/status"

# 0: Point 1, 1: Point 2, 2: Point 3, 3: Moving (post-calibration)
sim_state = 0


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
    # 位置：沿固定折线 (0,0)->(5,0)->(0,5)->(0,0) 循环行走。
    # 朝向：行走过程中每秒 2 次随机地向顺时针/逆时针转 30 度（不停下、与位置解耦）。
    LOOP_SEGMENTS = [
        (5.0, 0.0),
        (0.0, 5.0),
        (0.0, 0.0),
    ]

    WALK_SPEED_MPS = 1.0
    TURN_INTERVAL_SEC = 0.5     # 一秒 2 次转向
    TURN_ANGLE_DEG = 30.0       # 每次随机 ±30 度
    UPDATE_HZ = 10

    segment_idx = 0
    pos_x = 0.0
    pos_y = 0.0
    current_yaw = 0.0           # 单位：度，CCW 为正
    last_turn_sec = 0.0

    threading.Thread(target=poll_status, daemon=True).start()

    last_log_sec = -1

    try:
        while True:
            dt = 1.0 / UPDATE_HZ
            now_sec = time.time()
            state = sim_state

            if state < 3:
                tag_uwb_x, tag_uwb_y = CALIB_POINTS[state]
                yaw_to_send = 0.0
            else:
                # 位置：沿固定折线推进
                wx, wy = LOOP_SEGMENTS[segment_idx]
                dx = wx - pos_x
                dy = wy - pos_y
                dist = math.hypot(dx, dy)
                if dist <= WALK_SPEED_MPS * dt:
                    # 到点 → 切到下一段，继续走（不停止、不专门转圈）
                    pos_x, pos_y = wx, wy
                    segment_idx = (segment_idx + 1) % len(LOOP_SEGMENTS)
                else:
                    step = WALK_SPEED_MPS * dt
                    pos_x += (dx / dist) * step
                    pos_y += (dy / dist) * step

                # 朝向：每秒 2 次随机 ±30 度（与位置完全独立，边走边转）
                if now_sec - last_turn_sec >= TURN_INTERVAL_SEC:
                    last_turn_sec = now_sec
                    direction = random.choice([-1.0, 1.0])
                    current_yaw = (current_yaw + direction * TURN_ANGLE_DEG) % 360.0

                tag_uwb_x = pos_x
                tag_uwb_y = pos_y
                # IMU 朝向：UE 约定（与原代码一致，CCW 取反）
                yaw_to_send = (-current_yaw) % 360.0

            # UWB 位置一直发
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
                if state >= 3:
                    print(
                        f"[Sim] pos=({pos_x:.2f},{pos_y:.2f}) "
                        f"seg={segment_idx} yaw={current_yaw:.1f}°",
                        flush=True,
                    )

            time.sleep(dt)

    except KeyboardInterrupt:
        print("\n[Sim] Simulation stopped.")
        sock.close()


if __name__ == "__main__":
    print("==========================================================")
    print(" 综合校准与定位测试终端 (UWB + IMU via UDP)")
    print("==========================================================")
    print("1. 确保已启动 web_app.py")
    print("2. 运行 Unreal Engine 并自动连接")
    print("3. 校准阶段 (通过 UE 界面按钮触发):")
    print("   - 点1 对应 UWB P1(0,0)，朝向 0 度")
    print("   - 点2 对应 UWB P2(5,0)，朝向 0 度")
    print("   - 点3 对应 UWB P3(0,5)，朝向 0 度")
    print("   - UE 端点击解算后，进入循环运动测试")
    print("4. 测试阶段:")
    print("   - 位置: 沿固定折线 (0,0)->(5,0)->(0,5)->(0,0) 循环行走")
    print("   - 朝向: 行走中每秒 2 次随机向顺/逆时针转 30 度（边走边转）")
    print("==========================================================\n")
    start_simulation()
