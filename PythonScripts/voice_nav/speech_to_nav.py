"""
speech_to_nav.py
Microphone listener that uses OpenAI Whisper to transcribe speech, then
parses the transcript into navigation commands and sends them to UE5.

Uses sounddevice (instead of pyaudio) for Python 3.14 compatibility.
Supports Chinese and English voice commands.

改进点（vs 原版）:
  - 升级模型 tiny→small，中文识别率大幅提升
  - 固定语言为中文（zh），避免自动检测误判
  - 动态背景噪声校准，自动设置静默阈值
  - 适当延长采集窗口，给 Whisper 更多上下文
  - 添加 initial_prompt 提示词，引导 Whisper 识别导航词汇
  - 添加 condition_on_previous_text=False 防止幻觉重复
  - 日志输出更清晰，方便调试
"""

import sys
import time
import threading
import queue
import numpy as np
import whisper
import sounddevice as sd
import subprocess
import os
from command_parser import parse_navigation_command
from nav_tcp_client import send_command



# ============================================================
# Configuration  ← 在这里调整参数
# ============================================================
WHISPER_MODEL      = "small"   # tiny / base / small / medium / large
                               # small 对中文的识别率远好于 base，速度可接受
WHISPER_LANGUAGE   = "zh"      # 固定为中文；如需英文改 "en"，双语保持 None
SAMPLE_RATE        = 16000     # Whisper 要求 16kHz
BLOCK_DURATION_SEC = 0.3       # 每块采集时长（秒）
SILENCE_BLOCKS     = 8         # 连续静默块数后触发识别（8×0.3s = 2.4s）
MIN_SPEECH_BLOCKS  = 4         # 最少语音块数才触发识别（4×0.3s = 1.2s）
MAX_PHRASE_SEC     = 8         # 超过该时长强制触发识别
MICROPHONE_INDEX   = None      # None = 系统默认

# Whisper initial_prompt：列出所有可能的目标词，提升识别准确率
WHISPER_PROMPT = (
    "导航到工位，去会议桌，去沙发，去空调，去门，"
    "navigate to workstation, go to conference table, go to sofa, "
    "go to air conditioner, go to door, stop, 停止, 停下"
)

# 噪声校准：启动时采集这么多秒的环境音来自动确定静默阈值
CALIBRATION_SEC    = 2.0
NOISE_MULTIPLIER   = 3.0       # 阈值 = 背景噪声 RMS × 该系数


# ============================================================
# 加载 Whisper 模型
# ============================================================

def load_model():
    print(f"[Speech] Loading Whisper model '{WHISPER_MODEL}' (language='{WHISPER_LANGUAGE}')...")
    model = whisper.load_model(WHISPER_MODEL)
    print(f"[Speech] Whisper model loaded.")
    return model


# ============================================================
# 工具函数
# ============================================================

def rms(audio_block: np.ndarray) -> float:
    """计算音频块的 RMS（Root Mean Square）音量。"""
    return float(np.sqrt(np.mean(np.square(audio_block.astype(np.float32) / 32768.0))))


def calibrate_noise(stream_queue: queue.Queue, duration_sec: float) -> float:
    """
    采集 duration_sec 秒的环境音，计算背景噪声 RMS，
    返回推荐静默阈值（噪声 RMS × NOISE_MULTIPLIER）。
    """
    print(f"[Speech] Calibrating background noise for {duration_sec:.0f}s, please stay quiet...")
    num_blocks = int(duration_sec / BLOCK_DURATION_SEC)
    levels = []
    for _ in range(num_blocks):
        try:
            block = stream_queue.get(timeout=2.0)
            levels.append(rms(block))
        except queue.Empty:
            break

    if not levels:
        return 0.015  # 兜底默认值

    noise_rms = float(np.mean(levels))
    threshold = noise_rms * NOISE_MULTIPLIER
    # 最低保护阈值，避免极安静环境下任何声音都触发
    threshold = max(threshold, 0.008)
    print(f"[Speech] Background RMS={noise_rms:.4f} → Silence threshold={threshold:.4f}")
    return threshold


# ============================================================
# 主监听循环
# ============================================================

def start_listening(model, mic_index: int | None = MICROPHONE_INDEX):
    """开始麦克风监听，阻塞直到 Ctrl+C。"""
    block_size = int(SAMPLE_RATE * BLOCK_DURATION_SEC)
    audio_queue = queue.Queue()

    def callback(indata, frames, time_info, status):
        if status:
            print(f"[Speech] ⚠ sounddevice status: {status}")
        audio_queue.put(indata[:, 0].copy())  # 取单声道

    print("=" * 55)
    print("  Voice Navigation – Listening for commands")
    print("=" * 55)
    print(f"  Whisper model : {WHISPER_MODEL}")
    print(f"  Language      : {WHISPER_LANGUAGE or 'auto-detect'}")
    print(f"  UE5 port      : 9001")
    print(f"  Sample rate   : {SAMPLE_RATE} Hz")
    print()
    print("  Say commands like:")
    print("    '导航到工位'  /  'go to workstation'")
    print("    '去沙发'      /  'navigate to sofa'")
    print("    '停止'        /  'stop'")
    print()
    print("  Press Ctrl+C to quit.")
    print("=" * 55)

    device_kwargs = {}
    if mic_index is not None:
        device_kwargs["device"] = mic_index

    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="int16",
        blocksize=block_size,
        callback=callback,
        **device_kwargs,
    ):
        # 启动 TTS Server
        tts_script_path = os.path.join(os.path.dirname(__file__), "tts_server.py")
        subprocess.Popen([sys.executable, tts_script_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # 噪声校准
        silence_threshold = calibrate_noise(audio_queue, CALIBRATION_SEC)
        print(f"[Speech] Ready! Listening... (threshold={silence_threshold:.4f})")
        print(f"[Speech] Tip: Speak clearly and pause briefly between commands.\n")
        send_command({"action": "status", "text": "等待语音输入..."})

        phrase_blocks: list[np.ndarray] = []
        silent_count = 0
        speaking = False

        try:
            while True:
                try:
                    block = audio_queue.get(timeout=1.0)
                except queue.Empty:
                    continue

                level = rms(block)

                if level > silence_threshold:
                    # 有效语音
                    if not speaking:
                        print("[Speech] 🎤 Detected speech...", end="", flush=True)
                    phrase_blocks.append(block)
                    silent_count = 0
                    speaking = True
                elif speaking:
                    # 语音后的尾静默
                    phrase_blocks.append(block)
                    silent_count += 1

                    total_sec = len(phrase_blocks) * BLOCK_DURATION_SEC
                    should_trigger = (
                        silent_count >= SILENCE_BLOCKS
                        or total_sec >= MAX_PHRASE_SEC
                    )

                    if should_trigger:
                        print()  # 换行
                        if len(phrase_blocks) >= MIN_SPEECH_BLOCKS:
                            send_command({"action": "status", "text": "识别中..."})
                            audio_data = (
                                np.concatenate(phrase_blocks).astype(np.float32)
                                / 32768.0
                            )
                            threading.Thread(
                                target=_process_phrase,
                                args=(model, audio_data, silence_threshold),
                                daemon=True,
                            ).start()
                        else:
                            print("[Speech] Too short, ignored.")
                            send_command({"action": "status", "text": "等待语音输入..."})

                        phrase_blocks = []
                        silent_count = 0
                        speaking = False

        except KeyboardInterrupt:
            print("\n[Speech] Stopping...")


# ============================================================
# 识别与分发
# ============================================================

def _process_phrase(model: whisper.Whisper, audio_data: np.ndarray, threshold: float):
    """识别一段语音并将导航指令发送到 UE5。"""
    try:
        result = model.transcribe(
            audio_data,
            language=WHISPER_LANGUAGE,
            fp16=False,
            initial_prompt=WHISPER_PROMPT,       # 提示词引导识别
            condition_on_previous_text=False,    # 避免幻觉重复前文
            temperature=0.0,                     # 贪心解码，更确定
            no_speech_threshold=0.6,             # 低于此置信度视为无语音
            logprob_threshold=-1.0,              # 低质量结果直接过滤
        )
    except Exception as e:
        print(f"[Speech] Whisper error: {e}")
        return

    # 过滤无语音片段
    segments = result.get("segments", [])
    if not segments:
        return

    # 如果所有 segment 置信度都很低，跳过
    avg_logprob = float(np.mean([s.get("avg_logprob", -1) for s in segments]))
    no_speech_prob = float(np.mean([s.get("no_speech_prob", 1) for s in segments]))
    if no_speech_prob > 0.7 or avg_logprob < -1.2:
        print(f"[Speech] Low confidence (no_speech={no_speech_prob:.2f}, logprob={avg_logprob:.2f}), ignored.")
        return

    transcript = result.get("text", "").strip()
    if not transcript or len(transcript) < 2:
        return

    print(f"[Speech] Heard: 「{transcript}」  (logprob={avg_logprob:.2f})")

    command = parse_navigation_command(transcript)
    if command:
        print(f"[Speech] → Command: {command}")
        ok = send_command(command)
        print(f"[Speech] → {'Sent to UE5 ✓' if ok else 'Failed – is UE5 running?'}")
    else:
        print(f"[Speech] → Not a navigation command, ignored.")
    print()
    send_command({"action": "status", "text": "等待语音输入..."})


# ============================================================
# 独立运行入口
# ============================================================

if __name__ == "__main__":
    # 可选：传入麦克风设备序号，例如 python speech_to_nav.py 1
    mic_idx = int(sys.argv[1]) if len(sys.argv) > 1 else None
    m = load_model()
    start_listening(m, mic_idx)
