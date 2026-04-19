"""
web_app.py – Voice Navigation + UBeacon Backend
================================================
WebSocket   ws://host:8090/ws   → voice audio + TTS + UE navigation commands
REST        POST /api/set_target → set absolute UE target (x,y,z)
REST        POST /api/beacon     → UBeacon Tools webhook (rssi / distance)
"""

import json
import logging
import threading
import time

import numpy as np
from flask import Flask, jsonify, request
from flask_sock import Sock

from modules.speech_to_nav import generate_tts_audio, process_audio_to_command
from modules.location_calibration import UwbCalibrationManager

# ── App setup ──────────────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="[Server] %(message)s")
log = logging.getLogger("web_app")

# Silence Flask's default HTTP request logging (werkzeug) to prevent polling spam
logging.getLogger('werkzeug').setLevel(logging.ERROR)

app = Flask(__name__)
sock = Sock(app)

@app.before_request
def log_all_requests():
    if ENABLE_VERBOSE_LOGS and request.path != "/":
        log.info(f"[HTTP Request] {request.method} {request.path} | Data: {request.get_data(as_text=True)}")

# ── Configuration ───────────────────────────────────────────────────────────────
ENABLE_VERBOSE_LOGS = False  # 开关：将此处改为 True 即可打开所有被注释掉的调试日志！
HARDCODE_CALIBRATION_CORNERS = False  # 开关：开启时，校准采点1强制当做 UWB(0,0)，采点2强制 UWB(0, 8)
UWB_FILTER_ALPHA = 0.15  # 低通滤波系数：越小越平滑（0.05~0.3）。设为 1.0 关闭滤波。

# ── Shared state ───────────────────────────────────────────────────────────────
active_ws: dict = {}      # ws -> { 'ip': str, 'connected_at': float }
uwb_calibrator = UwbCalibrationManager()
_smoothed_ue_pos = None   # low-pass filtered UE position (x, y)

def broadcast(payload: dict):
    """Send JSON to all connected WebSocket clients."""
    msg = json.dumps(payload, ensure_ascii=False)
    dead = []
    for ws in list(active_ws.keys()):
        try:
            ws.send(msg)
        except Exception:
            dead.append(ws)
    for ws in dead:
        active_ws.pop(ws, None)


# ── REST Endpoints ─────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return "Voice Nav Server is Running"


@app.route("/api/beacon/inspect", methods=["POST"])
def beacon_inspect():
    """Debug: log and echo back the exact raw JSON that UBeacon Tools sends."""
    raw = request.get_json(force=True, silent=True) or {}
    if ENABLE_VERBOSE_LOGS:
        log.info(f"[BEACON INSPECT] Fields: {list(raw.keys())}")
        log.info(f"[BEACON INSPECT] Data:   {json.dumps(raw, ensure_ascii=False)}")
    return jsonify({"received": raw, "fields": list(raw.keys())})


@app.route("/api/set_target", methods=["POST"])
def set_target():
    """Set an absolute target coordinate for UE character navigation."""
    data = request.get_json(force=True)
    x, y, z = float(data.get("x", 0)), float(data.get("y", 0)), float(data.get("z", 0))
    broadcast({"type": "set_target", "x": x, "y": y, "z": z})
    if ENABLE_VERBOSE_LOGS:
        log.info(f"set_target → x={x} y={y} z={z} to {len(active_ws)} clients")
    return jsonify({"status": "ok", "clients": len(active_ws)})




@app.route("/api/calibrate/heading", methods=["POST"])
def calibrate_heading():
    """
    Set current IMU heading alignment. 
    Expected JSON: { "imu_yaw": float, "target_ue_yaw": float }
    """
    data = request.get_json(force=True)
    imu_yaw = float(data.get("imu_yaw", 0))
    # If target_ue_yaw is not provided, we assume the user is looking towards UE 0 (Forward X)
    target_ue_yaw = float(data.get("target_ue_yaw", 0))
    
    offset = uwb_calibrator.calibrate_heading(imu_yaw, target_ue_yaw)
    log.info(f"Heading aligned: IMU={imu_yaw:.1f} → UE={target_ue_yaw:.1f} (Offset: {offset:.1f})")
    
    # Broadcast current status to let clients know they are now calibrated
    broadcast_status()
    
    return jsonify({"status": "ok", "imu_offset": offset})


@app.route("/api/calibrate/point", methods=["POST"])
def calibrate_point():
    """Record current UE position and latest UWB position for 3-point calibration."""
    data = request.get_json(force=True)
    ue_x = float(data.get("x", 0))
    ue_y = float(data.get("y", 0))
    point_index = data.get("index", None)

    # ==== [修改] 强行覆盖实际 UWB 测算位置，用于桌面方便测试 ====
    if HARDCODE_CALIBRATION_CORNERS:
        if point_index == 0:
            uwb_calibrator.update_uwb_pos(0.0, 0.0)
            if ENABLE_VERBOSE_LOGS:
                log.info("【测试专用】已经将 采点1 的其实际 UWB 基站位置强行修正锁定为 (0.0, 0.0)")
        elif point_index == 1:
            uwb_calibrator.update_uwb_pos(8.0, 0.0)
            if ENABLE_VERBOSE_LOGS:
                log.info("【测试专用】已经将 采点2 的其实际 UWB 基站位置强行修正锁定为 (8.0, 0.0)")
        elif point_index == 2:
            uwb_calibrator.update_uwb_pos(0.0, 8.0)
            if ENABLE_VERBOSE_LOGS:
                log.info("【测试专用】已经将 采点3 的其实际 UWB 基站位置强行修正锁定为 (0.0, 8.0)")
    # ==============================================================

    valid_count, msg = uwb_calibrator.add_calibration_point(ue_x, ue_y, point_index)
    if "No UWB signal" in msg:
        return jsonify({"status": "error", "message": msg}), 400
        
    if ENABLE_VERBOSE_LOGS:
        log.info(msg)
    return jsonify({"status": "ok", "captured_count": valid_count})


@app.route("/api/calibrate/solve", methods=["POST"])
def calibrate_solve():
    """Solve the 3-point affine transformation matrix mapping UWB to Unreal Engine."""
    matrix, msg = uwb_calibrator.solve_transform()
    if matrix:
        if ENABLE_VERBOSE_LOGS:
            log.info(msg)
        broadcast_status()
        return jsonify({"status": "ok", "matrix": {k: (list(v) if isinstance(v, tuple) else v) for k, v in matrix.items()}})
    else:
        return jsonify({"status": "error", "message": msg})


@app.route("/api/calibrate/status", methods=["GET"])
def calibrate_status():
    """Returns the current calibration status including UWB and IMU details."""
    with uwb_calibrator.lock:
        valid_count = sum(1 for p in uwb_calibrator.calib_points if p is not None)
        is_pos_cal = uwb_calibrator.transform_matrix is not None
        imu_offset = uwb_calibrator.imu_offset
    return jsonify({
        "status": "ok", 
        "points": valid_count, 
        "is_calibrated": is_pos_cal,
        "imu_offset": imu_offset
    })

def broadcast_status():
    """Helper to broadcast current calibration state to all clients."""
    with uwb_calibrator.lock:
        valid_count = sum(1 for p in uwb_calibrator.calib_points if p is not None)
        is_pos_cal = uwb_calibrator.transform_matrix is not None
        imu_offset = uwb_calibrator.imu_offset
    broadcast({
        "type": "status_update",
        "is_calibrated": is_pos_cal,
        "is_heading_calibrated": uwb_calibrator.is_heading_calibrated,
        "imu_offset": imu_offset,
        "points": valid_count
    })


@app.route("/api/calibrate/clear", methods=["POST"])
def calibrate_clear():
    uwb_calibrator.clear()
    broadcast_status()
    if ENABLE_VERBOSE_LOGS:
        log.info("Calibration cleared and status broadcasted.")
    return jsonify({"status": "ok"})


# ── WebSocket Handler ──────────────────────────────────────────────────────────

active_ws = {}

@sock.route("/ws")
def ws_handler(ws):
    client_ip = request.remote_addr
    log.info(f"WebSocket client connected from {client_ip}")
    active_ws[ws] = {"ip": client_ip, "connected_at": time.time()}
    audio_buffer = bytearray()

    try:
        # Initial greeting and status sync
        with uwb_calibrator.lock:
            is_pos_cal = uwb_calibrator.transform_matrix is not None
            imu_offset = uwb_calibrator.imu_offset
            valid_pts = sum(1 for p in uwb_calibrator.calib_points if p is not None)

        ws.send(json.dumps({
            "type": "status", 
            "text": "已连接", 
            "is_calibrated": is_pos_cal,
            "is_heading_calibrated": uwb_calibrator.is_heading_calibrated,
            "imu_offset": imu_offset,
            "points": valid_pts
        }, ensure_ascii=False))

        while True:
            data = ws.receive()
            if data is None:
                break

            if isinstance(data, bytes):
                audio_buffer.extend(data)
                continue

            try:
                msg = json.loads(data)
            except Exception:
                continue

            msg_type = msg.get("type")

            if msg_type == "imu":
                raw_yaw = float(msg.get("yaw", 0.0))
                # Centralized IMU offset calculation
                corrected_yaw = uwb_calibrator.apply_imu_offset(raw_yaw)
                broadcast({"type": "set_rotation", "yaw": corrected_yaw})

            elif msg_type == "tts":
                text = msg.get("text", "").strip()
                timestamp = float(msg.get("timestamp", 0.0))
                if text:
                    threading.Thread(
                        target=_tts_worker, args=(ws, text, timestamp), daemon=True
                    ).start()

            elif msg_type == "end_of_speech":
                if len(audio_buffer) < 8000:
                    ws.send(json.dumps({"type": "status", "text": "太短，请重试"}))
                    audio_buffer.clear()
                    continue

                log.info(f"🎤 [VOICE] 接收到一段语音数据，时长：{len(audio_buffer) / 32000.0:.2f} 秒。开始进入识别流程...")
                ws.send(json.dumps({"type": "status", "text": "识别中..."}))
                pcm = np.frombuffer(bytes(audio_buffer), dtype=np.int16)
                audio_buffer.clear()
                threading.Thread(
                    target=_voice_worker, args=(ws, pcm), daemon=True
                ).start()

    except Exception as e:
        log.info(f"WS disconnect: {e}")
    finally:
        active_ws.pop(ws, None)


import struct

def _tts_worker(ws, text: str, timestamp: float = 0.0):
    """Generate TTS audio and stream PCM back over WebSocket with an 8-byte timestamp header."""
    pcm_bytes = generate_tts_audio(text)
    if pcm_bytes:
        try:
            # Prefix the binary audio with the 8-byte little-endian double float timestamp
            header = struct.pack('<d', timestamp)
            ws.send(header + pcm_bytes)
        except Exception as e:
            log.error(f"TTS WS send error: {e}")


def _voice_worker(ws, pcm: np.ndarray):
    """Run Whisper inference and broadcast navigation command."""
    log.info("🚀 [VOICE] 开始进行本地 Whisper 模型推理...")
    cmd, transcript = process_audio_to_command(pcm)

    if cmd:
        broadcast({"type": "navigate_to", "destination": cmd.get("target", "")})
        status_text = f"识别成功: {transcript}"
    else:
        status_text = f"无法识别指令: {transcript}"

    log.info(f"🎤 [VOICE] {status_text}")

    try:
        ws.send(json.dumps({"type": "status", "text": status_text, "heard": transcript, "success": cmd is not None}))
        time.sleep(1.0)
        ws.send(json.dumps({"type": "status", "text": "正在全时收音..."}))
    except Exception as e:
        log.error(f"WS send error: {e}")


def _udp_uwb_listener(port: int = 9003):
    """Listens continuously for UWB Tag JSON pushes over UDP."""
    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    log.info(f"UWB UDP Listener active on port {port}")
    
    while True:
        try:
            data, addr = sock.recvfrom(4096)
            raw_text = data.decode("utf-8", errors="ignore").strip()
            # 增加日志：打印收到的完整UDP信号及来源端口
            if ENABLE_VERBOSE_LOGS:
                log.info(f"[UDP] 从 {addr} 收到信号: {raw_text}")
            
            payload = json.loads(raw_text)
            
            if payload.get("name") == "Pos" and payload.get("deviceName", "").startswith("T"):
                device_data = payload.get("data", {})
                pos_array = device_data.get("pos")
                
                if pos_array and len(pos_array) >= 2:
                    current_x, current_y = float(pos_array[0]), float(pos_array[1])
                    
                    uwb_calibrator.update_uwb_pos(current_x, current_y)
                    
                    # Apply transform if we have an active calibration matrix
                    transformed = uwb_calibrator.transform_uwb_to_ue(current_x, current_y)
                    if transformed:
                        global _smoothed_ue_pos
                        raw_x, raw_y = transformed
                        if _smoothed_ue_pos is None:
                            _smoothed_ue_pos = (raw_x, raw_y)
                        else:
                            # Exponential moving average (low-pass filter)
                            sx = _smoothed_ue_pos[0] + UWB_FILTER_ALPHA * (raw_x - _smoothed_ue_pos[0])
                            sy = _smoothed_ue_pos[1] + UWB_FILTER_ALPHA * (raw_y - _smoothed_ue_pos[1])
                            _smoothed_ue_pos = (sx, sy)
                        ue_x, ue_y = _smoothed_ue_pos
                        broadcast({"type": "set_target", "x": ue_x, "y": ue_y, "z": 0, "calibrated": True})
        except Exception as e:
            pass

# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse
    import socket

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()

    # Automatically discover local IP to display to the user
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        local_ip = s.getsockname()[0]
        s.close()
    except Exception:
        local_ip = "127.0.0.1"

    log.info("")
    log.info("=" * 60)
    log.info(f"📱 手机端请输入此 IP: ws://{local_ip}:{args.port}/ws")
    log.info("=" * 60)
    log.info("")

    # Start UWB listener thread
    threading.Thread(target=_udp_uwb_listener, daemon=True).start()

    log.info(f"Starting server at http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, debug=False)
