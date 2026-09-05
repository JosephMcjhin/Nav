"""
web_app.py – Voice Navigation + UBeacon Backend
================================================
WebSocket   ws://host:8090/ws   → voice audio + TTS + UE navigation commands
REST        POST /api/set_target → set absolute UE target (x,y,z)
REST        POST /api/beacon     → UBeacon Tools webhook (rssi / distance)
"""

import json
import logging
import queue
import threading
import time
import base64
import os
from pathlib import Path
from logging.handlers import RotatingFileHandler

import numpy as np
from flask import Flask, jsonify, request
from flask_sock import Sock

from modules.command_parser import parse_navigation_command, DESTINATION_MAP
from modules.location_calibration import UwbCalibrationManager
from modules.tts_engine import get_engine as get_tts_engine

# ── App setup ──────────────────────────────────────────────────────────────────
LOG_DIR = Path(__file__).resolve().parent / "logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)
LOG_FILE = LOG_DIR / "voice_nav.log"
LOG_FORMAT = "[%(asctime)s] [%(levelname)s] [%(name)s] %(message)s"

# 控制台和滚动文件同时记录。root logger 会收集 web_app、tts_engine、Flask
# 以及其它后端模块的日志，单个文件达到 10 MB 后自动保留 5 个历史文件。
logging.basicConfig(
    level=logging.INFO,
    format=LOG_FORMAT,
    datefmt="%Y-%m-%d %H:%M:%S",
    handlers=[
        logging.StreamHandler(),
        RotatingFileHandler(
            LOG_FILE,
            maxBytes=10 * 1024 * 1024,
            backupCount=5,
            encoding="utf-8",
        ),
    ],
    force=True,
)
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
UWB_FILTER_ALPHA = 0.15  # 低通滤波系数：越小越平滑（0.05~0.3）。设为 1.0 关闭滤波。
UE_COMMAND_QUEUE_SIZE = 128
UE_STATE_SEND_HZ = 30.0

# ── Shared state ───────────────────────────────────────────────────────────────
active_ws: dict = {}      # ws -> { 'ip': str, 'connected_at': float }
uwb_calibrator = UwbCalibrationManager()
nav_request_client_ws = None
ue_client_ws = None
glasses_client_ws = None
last_missing_glasses_log = 0.0
last_no_ue_log = 0.0
imu_state_lock = threading.Lock()
last_imu_received_at = None
ue_command_queue: "queue.Queue[dict]" = queue.Queue(maxsize=UE_COMMAND_QUEUE_SIZE)
ue_latest_state: dict[str, dict] = {}
ue_latest_state_lock = threading.Lock()
ue_sender_event = threading.Event()

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


def send_json(ws, payload: dict) -> bool:
    """Send JSON to a specific WebSocket client."""
    if ws is None:
        return False
    try:
        ws.send(json.dumps(payload, ensure_ascii=False))
        return True
    except Exception:
        cleanup_ws(ws)
        return False


def _get_ue_targets() -> list:
    if ue_client_ws in active_ws:
        return [ue_client_ws]

    ue_clients = [ws for ws, meta in active_ws.items() if meta.get("role") == "ue"]
    if ue_clients:
        return ue_clients

    if len(active_ws) == 1:
        only_ws = next(iter(active_ws.keys()))
        meta = active_ws.get(only_ws, {})
        if ENABLE_VERBOSE_LOGS:
            log.info(
                f"No registered UE client; falling back to the only WebSocket "
                f"client ip={meta.get('ip', 'unknown')} role={meta.get('role', 'unregistered')}"
            )
        return [only_ws]

    return []


def _send_to_ue_now(payload: dict) -> bool:
    """Send JSON to UE from the single UE sender thread."""
    targets = _get_ue_targets()
    if not targets:
        _log_no_ue_client_once()
        return False

    sent = False
    for ws in targets:
        sent = send_json(ws, payload) or sent
    return sent


def _enqueue_ue_command(payload: dict) -> bool:
    if not _get_ue_targets():
        _log_no_ue_client_once()
        return False
    try:
        ue_command_queue.put_nowait(payload)
        ue_sender_event.set()
        return True
    except queue.Full:
        log.warning("UE command queue full; dropping payload=%s", payload.get("type"))
        return False


def _update_latest_ue_state(payload: dict) -> bool:
    if not _get_ue_targets():
        _log_no_ue_client_once()
        return False
    payload_type = payload.get("type")
    if not payload_type:
        return False
    with ue_latest_state_lock:
        ue_latest_state[payload_type] = payload
    ue_sender_event.set()
    return True


def send_to_ue(payload: dict, *, latest_state: bool = False) -> bool:
    """Queue JSON for UE without blocking sensor or request handler threads."""
    if latest_state:
        return _update_latest_ue_state(payload)
    return _enqueue_ue_command(payload)


def _pop_latest_ue_states() -> list[dict]:
    with ue_latest_state_lock:
        states = list(ue_latest_state.values())
        ue_latest_state.clear()
    return states


def _ue_sender_loop():
    min_state_interval = 1.0 / UE_STATE_SEND_HZ
    last_state_send = 0.0
    log.info(
        f"UE sender active: command_queue={UE_COMMAND_QUEUE_SIZE} "
        f"state_rate={UE_STATE_SEND_HZ:.0f}Hz"
    )
    while True:
        ue_sender_event.wait(timeout=min_state_interval)
        ue_sender_event.clear()

        while True:
            try:
                payload = ue_command_queue.get_nowait()
            except queue.Empty:
                break
            _send_to_ue_now(payload)

        now = time.time()
        if now - last_state_send < min_state_interval:
            continue

        for payload in _pop_latest_ue_states():
            _send_to_ue_now(payload)
        last_state_send = now


def start_ue_sender():
    threading.Thread(target=_ue_sender_loop, daemon=True, name="UESender").start()


def _log_no_ue_client_once():
    """Throttle the 'No UE client available' warning to once per 5s.

    test_server.py fires set_target / set_rotation many times per second,
    which would otherwise flood the log when UE is not connected.
    """
    global last_no_ue_log
    now = time.time()
    if now - last_no_ue_log >= 5.0:
        last_no_ue_log = now
        log.warning(
            f"No UE client available active_clients={len(active_ws)} "
            f"(throttled 5s)"
        )


def send_to_glasses(payload: dict) -> bool:
    """Send JSON only to the registered glasses client."""
    return send_json(glasses_client_ws, payload)


def register_ws_role(ws, role: str):
    """Track stable client roles so routing does not depend on nav requests."""
    global ue_client_ws, glasses_client_ws, nav_request_client_ws

    meta = active_ws.get(ws, {})
    if meta.get("role") == role:
        return

    if role == "ue":
        ue_client_ws = ws
    elif role == "glasses":
        glasses_client_ws = ws
        nav_request_client_ws = ws
    else:
        log.warning(f"Unknown WebSocket role from {active_ws.get(ws, {}).get('ip', 'unknown')}: {role}")
        return

    meta["role"] = role
    if role == "glasses":
        meta["last_imu_warning_at"] = 0.0
    log.info(f"WebSocket client registered: ip={meta.get('ip', 'unknown')} role={role}")


def get_ws_role(ws) -> str:
    return active_ws.get(ws, {}).get("role", "unregistered")


def cleanup_ws(ws):
    global ue_client_ws, glasses_client_ws, nav_request_client_ws

    meta = active_ws.get(ws, {})
    if meta:
        log.info(f"WebSocket client disconnected: ip={meta.get('ip', 'unknown')} role={meta.get('role', 'unregistered')}")

    if ue_client_ws is ws:
        ue_client_ws = None
    if glasses_client_ws is ws:
        glasses_client_ws = None
    if nav_request_client_ws is ws:
        nav_request_client_ws = None
    active_ws.pop(ws, None)


def log_missing_glasses_once():
    global last_missing_glasses_log

    now = time.time()
    if now - last_missing_glasses_log >= 5.0:
        last_missing_glasses_log = now
        log.warning("No active glasses client to receive TTS audio.")


def mark_imu_received(ws=None):
    """Record the latest valid IMU packet from WS or the external UDP gateway."""
    global last_imu_received_at
    now = time.time()
    with imu_state_lock:
        last_imu_received_at = now
    if ws is not None:
        meta = active_ws.get(ws)
        if meta is not None:
            meta["last_imu_at"] = now


def _imu_watchdog_loop():
    """Warn the connected glasses when no fresh IMU packet has arrived."""
    no_imu_timeout = 5.0
    warning_interval = 5.0
    log.info(
        "IMU watchdog active: timeout=%.1fs warning_interval=%.1fs",
        no_imu_timeout,
        warning_interval,
    )

    while True:
        time.sleep(1.0)
        now = time.time()
        ws = glasses_client_ws
        if ws is None or ws not in active_ws:
            continue

        meta = active_ws.get(ws, {})
        if meta.get("role") != "glasses":
            continue
        connected_at = float(meta.get("connected_at", now))
        last_warning_at = float(meta.get("last_imu_warning_at", 0.0))
        with imu_state_lock:
            last_imu_at = last_imu_received_at

        has_fresh_imu = last_imu_at is not None and last_imu_at >= connected_at
        if has_fresh_imu:
            continue
        if now - connected_at < no_imu_timeout:
            continue
        if now - last_warning_at < warning_interval:
            continue

        warning = {
            "type": "warning",
            "code": "imu_timeout",
            "text": "未检测到 IMU 数据，请检查 IMU 传感器连接",
        }
        if send_json(ws, warning):
            meta["last_imu_warning_at"] = now
            log.warning(
                "IMU watchdog: no fresh IMU data for %.1fs; warning sent to glasses ip=%s",
                now - connected_at if last_imu_at is None else now - last_imu_at,
                meta.get("ip", "unknown"),
            )


def _stream_and_send_tts(target_ws, text: str, timestamp: int):
    """Stream TTS audio chunks to the requesting client.

    Pushes a sequence of WS messages produced by the TTSEngine:
      nav_audio_start → nav_audio_chunk* → nav_audio_end
    The client plays chunks as they arrive (low first-byte latency).
    On synthesis failure the engine emits a nav_prompt text fallback.
    """
    t_recv = time.time()
    log.info(
        f"[TTS] request received: text={text[:40]!r} ts={timestamp} "
        f"backend=edge-tts"
    )

    engine = get_tts_engine()
    n_chunks = 0
    t_first_packet = None
    for msg in engine.build_ws_stream_messages(text, timestamp):
        if t_first_packet is None and msg.get("type") == "nav_audio_start":
            t_first_packet = time.time()
            log.info(
                f"[TTS] first packet (nav_audio_start) sent: "
                f"first-byte latency={( t_first_packet - t_recv) * 1000:.0f}ms"
            )
        if msg.get("type") == "nav_audio_chunk":
            n_chunks += 1
        send_json(target_ws, msg)

    total_ms = (time.time() - t_recv) * 1000
    log.info(
        f"[TTS] done: total={total_ms:.0f}ms chunks={n_chunks} "
        f"backend={engine.backend_name} ts={timestamp}"
    )



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
    send_to_ue({"type": "set_target", "x": x, "y": y, "z": z})
    if ENABLE_VERBOSE_LOGS:
        log.info(f"set_target → x={x} y={y} z={z} to {len(active_ws)} clients")
    return jsonify({"status": "ok", "ue_connected": ue_client_ws is not None})


@app.route("/api/stop_navigation", methods=["POST"])
def stop_navigation():
    """Immediately stop character navigation in UE."""
    send_to_ue({"type": "stop_navigation"})
    log.info("stop_navigation → Sent to UE client")
    return jsonify({"status": "ok", "ue_connected": ue_client_ws is not None})


@app.route("/api/calibrate/heading", methods=["POST"])
def calibrate_heading():
    """
    Set current IMU heading alignment. 
    Expected JSON: { "imu_yaw": float, "target_ue_yaw": float }
    """
    data = request.get_json(force=True)
    # We explicitly ignore the imu_yaw passed from UE to avoid axis mismatches (e.g. Tilt.Y vs Yaw)
    # The calibrator will use its internal 'last_imu_yaw' updated via WebSocket/UDP.
    target_ue_yaw = float(data.get("target_ue_yaw", 0))
    
    offset, msg = uwb_calibrator.calibrate_heading(None, target_ue_yaw)
    
    if offset is None:
        return jsonify({"status": "error", "message": msg}), 400

    log.info(msg)
    broadcast_status()
    return jsonify({"status": "ok", "imu_offset": offset})


@app.route("/api/calibrate/point", methods=["POST"])
def calibrate_point():
    """Record current UE position and latest UWB position for 3-point calibration."""
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
        "imu_offset": imu_offset,
        "is_imu_calibrated": uwb_calibrator.is_imu_calibrated
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
        "is_imu_calibrated": uwb_calibrator.is_imu_calibrated,
        "imu_offset": imu_offset,
        "points": valid_count
    })





# ── 远程导航参数配置 ───────────────────────────────────────────────────────────
# 路径：与 web_app.py 同目录下的 nav_config.json。每次拉取都重新读取，
# 这样运维可以直接改 JSON 无需重启服务。
NAV_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "nav_config.json")
_nav_config_mtime = 0.0          # 上次成功加载时的 mtime，用于增量缓存
_nav_config_cache = None         # 上次成功加载的内容


def _load_nav_config_payload():
    """读取 nav_config.json 并返回一个清理过的 dict（只含可下发字段）。

    返回 None 表示文件不存在或解析失败。
    缓存策略：仅当文件 mtime 变化时才重新解析；这样高频拉取不会有 IO 开销。
    """
    global _nav_config_mtime, _nav_config_cache

    if not os.path.isfile(NAV_CONFIG_PATH):
        log.warning(f"[nav_config] file not found: {NAV_CONFIG_PATH}")
        return None

    try:
        mtime = os.path.getmtime(NAV_CONFIG_PATH)
    except OSError:
        return None

    if _nav_config_cache is not None and mtime == _nav_config_mtime:
        return _nav_config_cache

    try:
        with open(NAV_CONFIG_PATH, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, ValueError) as e:
        log.error(f"[nav_config] failed to load: {e}")
        return None

    if not isinstance(raw, dict):
        log.error("[nav_config] root is not a JSON object")
        return None

    # 剥离注释字段（下划线开头的元信息，例如 _comment / _types）
    payload = {k: v for k, v in raw.items() if not str(k).startswith("_")}
    _nav_config_cache = payload
    _nav_config_mtime = mtime
    log.info(f"[nav_config] loaded {len(payload)} keys from {NAV_CONFIG_PATH}")
    return payload


@app.route("/api/calibrate/clear", methods=["POST"])
def calibrate_clear():
    uwb_calibrator.clear()
    broadcast_status()
    if ENABLE_VERBOSE_LOGS:
        log.info("Calibration cleared and status broadcasted.")
    return jsonify({"status": "ok"})


# ── WebSocket Handler ──────────────────────────────────────────────────────────



@sock.route("/ws")
def ws_handler(ws):
    global nav_request_client_ws
    client_ip = request.remote_addr
    log.info(f"WebSocket client connected: ip={client_ip} role=unregistered")
    active_ws[ws] = {"ip": client_ip, "connected_at": time.time(), "role": "unregistered"}
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
            "is_imu_calibrated": uwb_calibrator.is_imu_calibrated,
            "imu_offset": imu_offset,
            "points": valid_pts
        }, ensure_ascii=False))

        while True:
            data = ws.receive()
            if data is None:
                break

            try:
                msg = json.loads(data)
            except Exception:
                continue

            msg_type = msg.get("type")

            if msg_type == "register":
                role = str(msg.get("role", "")).strip().lower()
                register_ws_role(ws, role)
                send_json(ws, {"type": "status", "text": f"registered:{role}", "success": True})

            elif msg_type == "imu":
                raw_yaw = float(msg.get("yaw", 0.0))
                mark_imu_received(ws)
                # Update the latest raw IMU yaw for heading calibration
                uwb_calibrator.update_imu_yaw(raw_yaw)
                if (not uwb_calibrator.is_imu_calibrated or
                        uwb_calibrator.transform_matrix is None):
                    continue
                # Apply current offset and broadcast to UE
                corrected_yaw = uwb_calibrator.apply_imu_offset(raw_yaw)
                send_to_ue({"type": "set_rotation", "yaw": corrected_yaw}, latest_state=True)

            elif msg_type == "nav_request":
                if get_ws_role(ws) != "glasses":
                    log.info(f"Auto-registering WebSocket client as glasses from nav_request: ip={client_ip}")
                register_ws_role(ws, "glasses")
                target_str = msg.get("target", "")
                cmd = parse_navigation_command(target_str)
                if cmd:
                    nav_request_client_ws = ws
                    sent_to_ue = send_to_ue({"type": "navigate_to", "destination": cmd.get("target", "")})
                    log.info(
                        f"Navigation request target={cmd.get('target', '')} "
                        f"sent_to_ue={sent_to_ue} active_clients={len(active_ws)}"
                    )
                    send_json(ws, {"type": "status", "text": f"开始导航到: {cmd.get('target', '')}", "success": True})
                else:
                    send_json(ws, {"type": "status", "text": "无法识别目标", "success": False})

            elif msg_type == "stop_navigation":
                send_to_ue({"type": "stop_navigation"})
                log.info("Stop navigation command received from client.")

            elif msg_type == "nav_prompt":
                if get_ws_role(ws) != "ue":
                    log.info(f"Auto-registering WebSocket client as ue from nav_prompt: ip={client_ip}")
                register_ws_role(ws, "ue")
                text = msg.get("text", "")
                # Create a millisecond timestamp to help the client identify the latest prompt
                timestamp = int(time.time() * 1000)
                target_ws = glasses_client_ws or nav_request_client_ws
                if target_ws in active_ws:
                    threading.Thread(
                        target=_stream_and_send_tts,
                        args=(target_ws, text, timestamp),
                        daemon=True,
                    ).start()
                else:
                    log_missing_glasses_once()

            elif msg_type in ("nav_beep", "nav_drip", "nav_sound"):
                target_ws = glasses_client_ws or nav_request_client_ws
                if target_ws in active_ws:
                    send_json(target_ws, msg)
                else:
                    log_missing_glasses_once()

            # ── Calibration via WebSocket (代替 HTTP) ──────────────────────
            elif msg_type == "calibrate_point":
                ue_x = float(msg.get("x", 0))
                ue_y = float(msg.get("y", 0))
                point_index = msg.get("index", None)
                valid_count, result_msg = uwb_calibrator.add_calibration_point(ue_x, ue_y, point_index)
                if "No UWB signal" in result_msg:
                    send_json(ws, {"type": "calibrate_point_result", "status": "error", "message": result_msg})
                    log.warning(f"Calib point failed: {result_msg}")
                else:
                    log.info(result_msg)
                    broadcast_status()
                    send_json(ws, {"type": "calibrate_point_result", "status": "ok", "captured_count": valid_count})

            elif msg_type == "calibrate_solve":
                matrix, result_msg = uwb_calibrator.solve_transform()
                if matrix:
                    log.info(result_msg)
                    broadcast_status()
                    send_json(ws, {"type": "calibrate_solve_result", "status": "ok",
                                   "matrix": {k: (list(v) if isinstance(v, tuple) else v) for k, v in matrix.items()}})
                else:
                    send_json(ws, {"type": "calibrate_solve_result", "status": "error", "message": result_msg})
                    log.warning(f"Calib solve failed: {result_msg}")

            elif msg_type == "calibrate_heading":
                target_ue_yaw = float(msg.get("target_ue_yaw", 0))
                offset, result_msg = uwb_calibrator.calibrate_heading(None, target_ue_yaw)
                if offset is None:
                    send_json(ws, {"type": "calibrate_heading_result", "status": "error", "message": result_msg})
                    log.warning(f"Calib heading failed: {result_msg}")
                else:
                    log.info(result_msg)
                    broadcast_status()
                    send_json(ws, {"type": "calibrate_heading_result", "status": "ok", "imu_offset": offset})

            elif msg_type == "calibrate_clear":
                uwb_calibrator.clear()
                broadcast_status()
                log.info("Calibration cleared via WebSocket.")
                send_json(ws, {"type": "calibrate_clear_result", "status": "ok"})

            # ── 远程导航参数配置（UE 客户端拉取 nav_config.json） ─────────
            elif msg_type == "get_nav_config":
                config_payload = _load_nav_config_payload()
                if config_payload is None:
                    send_json(ws, {
                        "type": "nav_config_result",
                        "status": "error",
                        "message": "nav_config.json missing or invalid"
                    })
                else:
                    # 注意：原样回传 config 对象，键名需与 UE 侧 UPROPERTY 一致
                    send_json(ws, {
                        "type": "nav_config",
                        "status": "ok",
                        "config": config_payload
                    })
                    log.info(
                        f"nav_config sent to {client_ip}: "
                        f"{len(config_payload)} keys"
                    )



    except Exception as e:
        log.info(f"WS disconnect: {e}")
    finally:
        cleanup_ws(ws)





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
            
            # Handle external IMU data
            if "euler" in payload:
                euler = payload.get("euler")
                if isinstance(euler, list) and len(euler) >= 3:
                    raw_yaw = float(euler[0])
                    mark_imu_received()
                    # Update the latest raw IMU yaw for heading calibration
                    uwb_calibrator.update_imu_yaw(raw_yaw)
                    if (not uwb_calibrator.is_imu_calibrated or
                            uwb_calibrator.transform_matrix is None):
                        continue
                    # Apply current offset and broadcast to UE
                    corrected_yaw = uwb_calibrator.apply_imu_offset(raw_yaw)
                    send_to_ue({"type": "set_rotation", "yaw": corrected_yaw}, latest_state=True)
                continue
            
            if payload.get("name") == "Pos" and payload.get("deviceName", "").startswith("T"):
                device_data = payload.get("data", {})
                pos_array = device_data.get("pos")
                
                if pos_array and len(pos_array) >= 2:
                    current_x, current_y = float(pos_array[0]), float(pos_array[1])
                    
                    uwb_calibrator.update_uwb_pos(current_x, current_y)
                    if (uwb_calibrator.transform_matrix is None or
                            not uwb_calibrator.is_imu_calibrated):
                        continue

                    # Apply transform if we have an active calibration matrix
                    transformed = uwb_calibrator.transform_uwb_to_ue(current_x, current_y)
                    if transformed:
                        ue_x, ue_y = transformed
                        send_to_ue({
                            "type": "set_target",
                            "x": ue_x,
                            "y": ue_y,
                            "z": 0,
                            "calibrated": True
                        }, latest_state=True)
        except Exception as e:
            pass

# ── Entry point ────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse
    import socket

    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument(
        "--tts-backend",
        choices=["edge", "local"],
        default="edge",
        help="TTS 后端：edge=云端 edge-tts（默认，质量好需联网）；"
             "local=本地 PowerShell System.Speech（零延迟机械音）",
    )
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

    # Start outbound UE sender before sensor traffic begins.
    start_ue_sender()

    # Start UWB listener thread
    threading.Thread(target=_udp_uwb_listener, daemon=True).start()
    threading.Thread(target=_imu_watchdog_loop, daemon=True, name="IMUWatchdog").start()

    # 启动时立即初始化 TTS 引擎。
    # 缺库/后端不可用会在这里直接抛异常退出，而不是等到第一次 TTS 请求才崩。
    log.info(f"Initializing TTS engine (backend={args.tts_backend})...")
    try:
        eng = get_tts_engine(backend=args.tts_backend)
        log.info(f"TTS engine ready: backend={eng.backend_name}")
    except Exception as e:
        log.error("=" * 60)
        log.error(f"TTS 引擎初始化失败，服务无法启动:\n{e}")
        log.error("=" * 60)
        raise

    log.info(f"Log file: {LOG_FILE}")
    log.info(f"Starting server at http://{args.host}:{args.port}")
    app.run(host=args.host, port=args.port, debug=False)
