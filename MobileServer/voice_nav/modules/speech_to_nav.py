"""
speech_to_nav.py
Handles TTS generation (edge-tts) and Speech recognition (Whisper / OpenAI).
"""

import asyncio
import logging
import os
import tempfile
import threading
import edge_tts
import numpy as np
import whisper
from pydub import AudioSegment

from modules.command_parser import parse_navigation_command

log = logging.getLogger("speech_to_nav")

# ── Configuration ──────────────────────────────────────────────────────────────
WHISPER_MODEL    = "medium"   
WHISPER_LANGUAGE = "zh"      
WHISPER_PROMPT   = "导航 前往 左转 右转 停止 目的地"

_model = None
_model_lock = threading.Lock()
_inference_lock = threading.Lock()

def get_model():
    global _model
    if _model is None:
        with _model_lock:
            if _model is None:
                log.info(f"Loading Whisper model '{WHISPER_MODEL}'...")
                _model = whisper.load_model(WHISPER_MODEL)
                log.info("Whisper model loaded.")
    return _model

def generate_tts_audio(text: str) -> bytes | None:
    """Generate TTS audio via edge-tts and return PCM raw_data bytes."""
    tmp_path = None
    try:
        log.info(f"Generating TTS: [{text}]")
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
        return audio.raw_data
    except Exception as e:
        log.error(f"TTS error: {e}")
        return None
    finally:
        if tmp_path and os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def process_audio_to_command(pcm_data: np.ndarray) -> tuple[dict | None, str]:
    """
    Run Whisper transcription and parse navigation command. 
    Returns (cmd_dict | None, transcript).
    """
    with _inference_lock:
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
            log.error(f"Whisper inference error: {e}")
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
        if cmd:
            log.info(f"Command parsed: {cmd}")
        else:
            log.info(f"Command not recognized from: [{transcript}]")
            
        return cmd, transcript
