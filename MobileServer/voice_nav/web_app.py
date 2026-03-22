"""
web_app.py – Voice Navigation + UBeacon Backend
================================================
WebSocket   ws://host:8090/ws   → voice audio + TTS + UE navigation commands
REST        POST /api/set_target → set absolute UE target (x,y,z)
REST        POST /api/beacon     → UBeacon Tools webhook (rssi / distance)
"""

import asyncio
import json
import logging
import math
import os
import tempfile
import threading
import time

import edge_tts
import numpy as np
from flask import Flask, jsonify, request
from flask_sock import Sock
from pydub import AudioSegment

from command_parser import parse_navigation_command
from speech_to_nav import WHISPER_LANGUAGE, load_model

# ── App setup ──────────────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="[Server] %(message)s")
log = logging.getLogger("web_app")

app = Flask(__name__)
sock = Sock(app)

# ── Shared state ───────────────────────────────────────────────────────────────
active_ws: set = set()
_model = None
_model_lock = threading.Lock()
_inference_lock = threading.Lock()

# UWB 2-Point Tracking & Calibration state
_uwb_lock = threading.Lock()
_last_uwb_pos: tuple[float, float] | None = None
_calib_points: list[dict] = []  # List of {"ue": (x,y), "uwb": (x,y)}
_transform_matrix: dict | None = None  # {"E1": (x,y), "W1": (x,y), "S": float, "theta": float}

WHISPER_PROMPT = "导航 前往 左转 右转 停止 目的地"


# ── Helpers ────────────────────────────────────────────────────────────────────

def get_model():
    global _model
    if _model is None:
        with _model_lock:
            if _model is None:
                log.info("Loading Whisper model...")
                _model = load_model()
    return _model


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


def solve_2point_transform(ue_points, uwb_points):
    """
    Given two points in UE space and two points in UWB space, calculate scale, rotation, and translation.
    Returns: {"E1": (ue_x1, ue_y1), "W1": (uwb_x1, uwb_y1), "S": scale, "theta": angle_rad}
    """
    (ue1_x, ue1_y), (ue2_x, ue2_y) = ue_points
    (uwb1_x, uwb1_y), (uwb2_x, uwb2_y) = uwb_points

    d_ue_x = ue2_x - ue1_x
    d_ue_y = ue2_y - ue1_y
    d_uwb_x = uwb2_x - uwb1_x
    d_uwb_y = uwb2_y - uwb1_y

    dist_ue = math.hypot(d_ue_x, d_ue_y)
    dist_uwb = math.hypot(d_uwb_x, d_uwb_y)

    if dist_uwb < 0.001:
        return None  # UWB points too close

    scale = dist_ue / dist_uwb
    angle_ue = math.atan2(d_ue_y, d_ue_x)
    angle_uwb = math.atan2(d_uwb_y, d_uwb_x)
    theta = angle_ue - angle_uwb

    return {"E1": (ue1_x, ue1_y), "W1": (uwb1_x, uwb1_y), "S": scale, "theta": theta}

def transform_uwb_to_ue(uwb_x, uwb_y, matrix):
    """Apply the transform matrix to a UWB coordinate to get UE coordinate."""
    e1_x, e1_y = matrix["E1"]
    w1_x, w1_y = matrix["W1"]
    s = matrix["S"]
    theta = matrix["theta"]

    dx = uwb_x - w1_x
    dy = uwb_y - w1_y

    # Rotate and Scale
    rx = dx * math.cos(theta) - dy * math.sin(theta)
    ry = dx * math.sin(theta) + dy * math.cos(theta)

    return (e1_x + s * rx, e1_y + s * ry)


def process_audio(pcm_data: np.ndarray):
    """Whisper transcription + command parsing. Returns (cmd | None, transcript)."""
    audio_f32 = pcm_data.astype(np.float32) / 32768.0
    try:
        result = get_model().transcribe(
            audio_f32,
            language=WHISPER_LANGUAGE,
            fp16=False,
            initial_prompt=WHISPER_PROMPT,
            condition_on_previous_text=False,
            temperature=0.0,
            no_speech_threshold=0.6,
            logprob_threshold=-1.0,
        )
    except Exception as e:
        log.error(f"Whisper error: {e}")
        return None, ""

    segs = result.get("segments", [])
    if not segs:
        return None, ""

    no_sp = float(np.mean([s.get("no_speech_prob", 1) for s in segs]))
    avg_lp = float(np.mean([s.get("avg_logprob", -1) for s in segs]))
    if no_sp > 0.7 or avg_lp < -1.2:
        return None, ""

    transcript = result.get("text", "").strip()
    if len(transcript) < 2:
        return None, transcript

    log.info(f"Heard: [{transcript}]")
    cmd = parse_navigation_command(transcript)
    log.info(f"Command: {cmd}" if cmd else f"Not recognized: [{transcript}]")
    return cmd, transcript


# ── REST Endpoints ─────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return "Voice Nav Server is Running"


@app.route("/api/beacon/inspect", methods=["POST"])
def beacon_inspect():
    """Debug: log and echo back the exact raw JSON that UBeacon Tools sends."""
    raw = request.get_json(force=True, silent=True) or {}
    log.info(f"[BEACON INSPECT] Fields: {list(raw.keys())}")
    log.info(f"[BEACON INSPECT] Data:   {json.dumps(raw, ensure_ascii=False)}")
    return jsonify({"received": raw, "fields": list(raw.keys())})


@app.route("/api/set_target", methods=["POST"])
def set_target():
    """Set an absolute target coordinate for UE character navigation."""
    data = request.get_json(force=True)
    x, y, z = float(data.get("x", 0)), float(data.get("y", 0)), float(data.get("z", 0))
    broadcast({"type": "set_target", "x": x, "y": y, "z": z})
    log.info(f"set_target → x={x} y={y} z={z} to {len(active_ws)} clients")
    return jsonify({"status": "ok", "clients": len(active_ws)})


@app.route("/api/calibrate/point", methods=["POST"])
def calibrate_point():
    """
    Record current UE position and latest UWB position for 2-point calibration.
    Expected: { "x": float, "y": float, "index": int (0 or 1) }
    If index is provided, overwrites that specific slot; otherwise appends.
    """
    data = request.get_json(force=True)
    ue_x = float(data.get("x", 0))
    ue_y = float(data.get("y", 0))
    # Optional: index 0 or 1 selects which calibration slot to overwrite
    point_index = data.get("index", None)

    with _uwb_lock:
        if _last_uwb_pos is None:
            return jsonify({"status": "error", "message": "No UWB signal received yet. Please wait for UWB data."}), 400

        pair = {"ue": (ue_x, ue_y), "uwb": _last_uwb_pos}

        if point_index is not None:
            idx = int(point_index)
            # Ensure list is long enough
            while len(_calib_points) <= idx:
                _calib_points.append(None)
            _calib_points[idx] = pair
            log.info(f"Calib sample[{idx}] overwrite: UE({ue_x:.1f}, {ue_y:.1f}) ↔ UWB{_last_uwb_pos}")
        else:
            if len(_calib_points) >= 2:
                # Auto-reset and start fresh so user can re-calibrate without explicit Reset
                _calib_points.clear()
                log.info("Auto-cleared old calibration points to allow re-capture.")
            _calib_points.append(pair)
            log.info(f"Calib sample [{len(_calib_points)}]: UE({ue_x:.1f}, {ue_y:.1f}) ↔ UWB{_last_uwb_pos}")

    valid_count = sum(1 for p in _calib_points if p is not None)
    return jsonify({"status": "ok", "captured_count": valid_count})


@app.route("/api/calibrate/solve", methods=["POST"])
def calibrate_solve():
    """Solve the 2-point Transformation matrix mapping UWB to Unreal Engine."""
    global _transform_matrix
    with _uwb_lock:
        valid_pts = [p for p in _calib_points if p is not None]
        if len(valid_pts) < 2:
            return jsonify({"status": "error", "message": f"Need 2 points to solve, got {len(valid_pts)}."})

        ue_pts = (valid_pts[0]["ue"], valid_pts[1]["ue"])
        uwb_pts = (valid_pts[0]["uwb"], valid_pts[1]["uwb"])

        matrix = solve_2point_transform(ue_pts, uwb_pts)
        if matrix:
            _transform_matrix = matrix
            log.info(f"Transformation SOLVED: Scale={matrix['S']:.3f}, Theta={matrix['theta']:.3f} rad")
            return jsonify({"status": "ok", "matrix": {k: (list(v) if isinstance(v, tuple) else v) for k, v in matrix.items()}})
        else:
            return jsonify({"status": "error", "message": "Math error (UWB points too close? Move to a different location for each capture.)"})


@app.route("/api/calibrate/clear", methods=["POST"])
def calibrate_clear():
    global _transform_matrix
    with _uwb_lock:
        _calib_points.clear()
        _transform_matrix = None
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

            # -- Binary: accumulate raw PCM audio --
            if isinstance(data, bytes):
                audio_buffer.extend(data)
                continue

            # -- Text: JSON control messages --
            try:
                msg = json.loads(data)
            except Exception:
                continue

            msg_type = msg.get("type")

            if msg_type == "tts":
                # Cloud TTS request: generate speech and stream PCM back
                text = msg.get("text", "").strip()
                if text:
                    threading.Thread(
                        target=_tts_worker, args=(ws, text), daemon=True
                    ).start()

            elif msg_type == "end_of_speech":
                # Process the buffered PCM audio through Whisper
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
    """Generate TTS audio via edge-tts and stream PCM back over WebSocket."""
    tmp_path = None
    try:
        log.info(f"TTS: [{text}]")
        communicate = edge_tts.Communicate(text, "zh-CN-XiaoxiaoNeural")
        with tempfile.NamedTemporaryFile(delete=False, suffix=".mp3") as f:
            tmp_path = f.name
        asyncio.run(communicate.save(tmp_path))
        audio = (
            AudioSegment.from_file(tmp_path)
            .set_channels(1)
            .set_frame_rate(16000)
            .set_sample_width(2)
        )
        ws.send(audio.raw_data)
    except Exception as e:
        log.error(f"TTS error: {e}")
    finally:
        if tmp_path and os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def _voice_worker(ws, pcm: np.ndarray):
    """Run Whisper inference and broadcast navigation command."""
    with _inference_lock:
        cmd, transcript = process_audio(pcm)

    if cmd:
        broadcast({"type": "navigate_to", "destination": cmd.get("target", "")})
        status_text = f"识别成功: {transcript}"
    else:
        status_text = f"无法识别指令: {transcript}"

    try:
        ws.send(json.dumps({"type": "status", "text": status_text, "heard": transcript, "success": cmd is not None}))
        time.sleep(1.0)
        ws.send(json.dumps({"type": "status", "text": "按住说话"}))
    except Exception as e:
        log.error(f"WS send error: {e}")


def _udp_uwb_listener(port: int = 9003):
    """Listens continuously for UWB Tag JSON pushes over UDP."""
    import socket
    global _last_uwb_pos
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    log.info(f"UWB UDP Listener active on port {port}")
    
    while True:
        try:
            data, addr = sock.recvfrom(4096)
            payload = json.loads(data.decode("utf-8").strip())
            
            # Mock UBeacon or Nooploop format: {"name": "Pos", "data": {"pos": [x, y, z]}}
            if payload.get("name") == "Pos" and "data" in payload:
                pos_array = payload["data"].get("pos")
                if pos_array and len(pos_array) >= 2:
                    current_x, current_y = float(pos_array[0]), float(pos_array[1])
                    
                    with _uwb_lock:
                        _last_uwb_pos = (current_x, current_y)
                        
                        # Apply transform if we have an active calibration matrix
                        if _transform_matrix:
                            ue_x, ue_y = transform_uwb_to_ue(current_x, current_y, _transform_matrix)
                            broadcast({"type": "set_target", "x": ue_x, "y": ue_y, "z": 0, "calibrated": True})
        except Exception as e:
            # log.debug(f"UDP parsing error: {e}")
            pass

# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    args = parser.parse_args()

    # Start UWB listener thread
    threading.Thread(target=_udp_uwb_listener, daemon=True).start()

    log.info(f"Starting server at http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, debug=False)
