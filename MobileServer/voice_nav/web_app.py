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

# ── Shared state ───────────────────────────────────────────────────────────────
active_ws: set = set()
uwb_calibrator = UwbCalibrationManager()

def broadcast(payload: dict):
    """Send JSON to all connected WebSocket clients."""
    msg = json.dumps(payload, ensure_ascii=False)
    dead = set()
    for ws in list(active_ws):
        try:
            ws.send(msg)
        except Exception:
            dead.add(ws)
    active_ws.difference_update(dead)


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


@app.route("/api/calibrate/point", methods=["POST"])
def calibrate_point():
    """Record current UE position and latest UWB position for 2-point calibration."""
    data = request.get_json(force=True)
    ue_x = float(data.get("x", 0))
    ue_y = float(data.get("y", 0))
    point_index = data.get("index", None)

    valid_count, msg = uwb_calibrator.add_calibration_point(ue_x, ue_y, point_index)
    if "No UWB signal" in msg:
        return jsonify({"status": "error", "message": msg}), 400
        
    if ENABLE_VERBOSE_LOGS:
        log.info(msg)
    return jsonify({"status": "ok", "captured_count": valid_count})


@app.route("/api/calibrate/solve", methods=["POST"])
def calibrate_solve():
    """Solve the 2-point Transformation matrix mapping UWB to Unreal Engine."""
    matrix, msg = uwb_calibrator.solve_transform()
    if matrix:
        if ENABLE_VERBOSE_LOGS:
            log.info(msg)
        return jsonify({"status": "ok", "matrix": {k: (list(v) if isinstance(v, tuple) else v) for k, v in matrix.items()}})
    else:
        return jsonify({"status": "error", "message": msg})


@app.route("/api/calibrate/status", methods=["GET"])
def calibrate_status():
    """Returns the current number of calibration points and whether calibration is solved."""
    with uwb_calibrator.lock:
        valid_count = sum(1 for p in uwb_calibrator.calib_points if p is not None)
        is_cal = uwb_calibrator.transform_matrix is not None
    return jsonify({"status": "ok", "points": valid_count, "is_calibrated": is_cal})


@app.route("/api/calibrate/clear", methods=["POST"])
def calibrate_clear():
    uwb_calibrator.clear()
    if ENABLE_VERBOSE_LOGS:
        log.info("Calibration cleared.")
    return jsonify({"status": "ok"})


# ── WebSocket Handler ──────────────────────────────────────────────────────────

@sock.route("/ws")
def ws_handler(ws):
    log.info("WebSocket client connected.")
    active_ws.add(ws)
    audio_buffer = bytearray()

    try:
        ws.send(json.dumps({"type": "status", "text": "已连接"}))

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

            if msg_type == "tts":
                text = msg.get("text", "").strip()
                if text:
                    threading.Thread(
                        target=_tts_worker, args=(ws, text), daemon=True
                    ).start()

            elif msg_type == "end_of_speech":
                if len(audio_buffer) < 8000:
                    ws.send(json.dumps({"type": "status", "text": "太短，请重试"}))
                    audio_buffer.clear()
                    continue

                ws.send(json.dumps({"type": "status", "text": "识别中..."}))
                pcm = np.frombuffer(bytes(audio_buffer), dtype=np.int16)
                audio_buffer.clear()
                threading.Thread(
                    target=_voice_worker, args=(ws, pcm), daemon=True
                ).start()

    except Exception as e:
        log.info(f"WS disconnect: {e}")
    finally:
        active_ws.discard(ws)


def _tts_worker(ws, text: str):
    """Generate TTS audio and stream PCM back over WebSocket."""
    pcm_bytes = generate_tts_audio(text)
    if pcm_bytes:
        try:
            ws.send(pcm_bytes)
        except Exception as e:
            log.error(f"TTS WS send error: {e}")


def _voice_worker(ws, pcm: np.ndarray):
    """Run Whisper inference and broadcast navigation command."""
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
        ws.send(json.dumps({"type": "status", "text": "按住说话"}))
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
            
            if payload.get("name") == "Device" and "data" in payload:
                device_data = payload["data"]
                device_name = device_data.get("name", "")
                
                # 过滤逻辑：只有名字以 T 开头（代表 Tag）的才处理，U 开头（代表 UWB Anchor）则忽略
                if not device_name.startswith("T"):
                    continue
                
                coord_obj = device_data.get("coordinate", {})
                pos_array = coord_obj.get("coords")
                
                if pos_array and len(pos_array) >= 2:
                    current_x, current_y = float(pos_array[0]), float(pos_array[1])
                    
                    uwb_calibrator.update_uwb_pos(current_x, current_y)
                    
                    # Apply transform if we have an active calibration matrix
                    transformed = uwb_calibrator.transform_uwb_to_ue(current_x, current_y)
                    if transformed:
                        ue_x, ue_y = transformed
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
