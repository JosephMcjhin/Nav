"""
tts_engine.py – 流式 TTS 引擎封装（edge-tts 后端）
================================================

唯一后端：**edge-tts**（微软 Azure 公开端点，云端流式 MP3）。
首字延迟通常 300~600ms（取决于网络），但流式下发后前端可边收边播。

工作原理
--------
1. edge-tts 异步流式返回 MP3 分片。
2. 攒够完整 MP3 后用 pydub 解码为 PCM（MP3 必须按帧累积解码）。
3. 重采样到统一规格 24000Hz / 16bit / Mono。
4. 切成 ~200ms 的分块，按 `nav_audio_start / nav_audio_chunk / nav_audio_end`
   协议推给前端，前端可边收边播。

对外接口
--------
TTSEngine()
    .build_ws_stream_messages(text, timestamp) -> Iterator[dict]
        产出可直接通过 WS 推送的消息字典序列（带 base64 音频）。
    .stream_pcm_chunks(text) -> Iterator[(bytes, bool)]
        产出 (pcm_bytes, cached) 元组序列，供自定义传输用。

唯一后端是 edge-tts，无回退、无环境变量切换、无模型下载。
首次 import 即触发 edge_tts 导入；缺库直接 ImportError，服务无法启动。

WebSocket 推送协议（流式）
--------------------------
首包:  {"type":"nav_audio_start","text":..., "timestamp":..., "sample_rate":24000,
        "sample_width":2, "channels":1, "backend":"edge-tts"}
中包:  {"type":"nav_audio_chunk","seq":0,"audio":"<base64-pcm>","timestamp":...,
        "cached":false}
尾包:  {"type":"nav_audio_end","timestamp":..., "backend":"edge-tts",
        "cached":false, "chunks":N}
"""

from __future__ import annotations

import asyncio
import base64
import io
import logging
import os
import subprocess
import sys
import tempfile
import threading
import wave
from collections import OrderedDict
from typing import Iterator, Optional

# 强制依赖：edge-tts 必须安装，否则本模块直接 ImportError 加载失败，
# 服务无法启动。
import edge_tts
from pydub import AudioSegment

log = logging.getLogger("tts_engine")

# ── 后端选择 ───────────────────────────────────────────────────────────────
# "edge"  → 云端 edge-tts（流式，质量好，需联网）
# "local" → 本地 Windows SAPI via PowerShell（零延迟，零网络，机械音）
# 实际用哪个由 web_app.py 命令行参数 --tts-backend 决定；
# 此处仅是 get_engine() 不带参数时的回退默认值。
DEFAULT_BACKEND = "edge"
IS_WINDOWS = sys.platform.startswith("win")

# ── 默认音频规格（与前端约定一致） ──────────────────────────────────────────
DEFAULT_SAMPLE_RATE = 24000
DEFAULT_SAMPLE_WIDTH = 2  # 16-bit
DEFAULT_CHANNELS = 1      # Mono
# 每段 PCM 大约 ~200ms，足够小可显著降低首字延迟，又不会让 WS 包数过多
STREAM_CHUNK_MS = 200

# ── LRU 缓存配置 ───────────────────────────────────────────────────────────
CACHE_ENABLED = os.environ.get("TTS_CACHE", "1") != "0"
CACHE_MAX_ENTRIES = 128

# 默认中文语音（微软 Neural 语音，质量好）
DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"

# 后端统一固定语速：1.0=正常，2.5=约 2.5 倍。
TTS_SPEED_MULTIPLIER = 2.5
EDGE_TTS_RATE = f"{round((TTS_SPEED_MULTIPLIER - 1.0) * 100):+d}%"
LOCAL_SAPI_RATE = max(
    -10, min(10, round((TTS_SPEED_MULTIPLIER - 1.0) * 5))
)  # SAPI Rate，范围 -10 ~ +10


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  LRU 缓存                                                                   ║
# ╚══════════════════════════════════════════════════════════════════════════╝
class _TTSCache:
    """线程安全的 LRU 缓存，key 为 text，value 为完整 PCM bytes。"""

    def __init__(self, max_entries: int = CACHE_MAX_ENTRIES):
        self._store: "OrderedDict[str, bytes]" = OrderedDict()
        self._lock = threading.Lock()
        self._max = max_entries

    def get(self, text: str) -> Optional[bytes]:
        with self._lock:
            if text not in self._store:
                return None
            self._store.move_to_end(text)
            return self._store[text]

    def put(self, text: str, pcm: bytes) -> None:
        if not pcm:
            return
        with self._lock:
            self._store[text] = pcm
            self._store.move_to_end(text)
            while len(self._store) > self._max:
                self._store.popitem(last=False)

    @property
    def size(self) -> int:
        with self._lock:
            return len(self._store)


_cache = _TTSCache()


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  后端实现：edge-tts（云端流式，唯一后端）                                    ║
# ╚══════════════════════════════════════════════════════════════════════════╝
class _EdgeTTSBackend:
    """使用微软 edge-tts（云端）合成语音，真正的增量流式。

    edge-tts 返回的是分块 MP3 流。MP3 必须按帧累积才能解码，
    单个小 chunk 无法独立解码。所以采用「攒够一段 MP3（约 1 秒）
    就立即解码推送 PCM 分段」的策略，而不是等全部 MP3 收完。

    这样首字延迟 ≈ 第一段 MP3 到达时间（通常 300~600ms），
    而不是整段合成时间（可能 2~3 秒）。
    """

    name = "edge-tts"
    is_local = False
    is_streaming_supported = True

    # 攒够这么多 MP3 字节就解码推送一段。
    # edge-tts 每个 chunk ~720B（128kbps MP3 ≈ 0.045 秒），
    # 4096B ≈ 0.26 秒音频，是 MP3 能稳定增量解码的最小量级。
    MP3_FLUSH_BYTES = 4096
    # MP3 解码需要前面帧的上下文，每次解码带上前一段末尾少许数据避免爆音
    MP3_OVERLAP_BYTES = 2048

    def __init__(self, voice: str = DEFAULT_VOICE):
        self.voice = voice

    @property
    def available(self) -> bool:
        return True  # edge_tts 已在顶部 import，能加载就可用

    async def _iter_mp3_chunks(self, text: str):
        """异步迭代 edge-tts 的 MP3 音频 chunk（原始 bytes）。"""
        communicate = edge_tts.Communicate(
            text, self.voice, rate=EDGE_TTS_RATE
        )
        async for chunk in communicate.stream():
            if chunk.get("type") == "audio":
                yield chunk["data"]

    def stream_pcm_chunks(self, text: str) -> Iterator[bytes]:
        """真正的增量流式：边收 MP3 边解码边推送 PCM 段。

        策略：MP3 累积到 MP3_FLUSH_BYTES 就用 pydub 解码一次，
        重采样/切分成 ~200ms PCM 段逐段 yield。每段解码带上前一段末尾
        MP3_OVERLAP_BYTES 的重叠，避免段间爆音。
        """
        chunk_bytes = DEFAULT_SAMPLE_RATE * DEFAULT_SAMPLE_WIDTH * \
            DEFAULT_CHANNELS * STREAM_CHUNK_MS // 1000

        # 用一个独立事件循环驱动 edge-tts 的异步迭代器
        loop = asyncio.new_event_loop()
        try:
            ait = self._iter_mp3_chunks(text).__aiter__()
            mp3_buffer = bytearray()
            prev_tail = b""  # 上一段末尾，用于重叠避免爆音

            while True:
                try:
                    mp3_piece = loop.run_until_complete(ait.__anext__())
                except StopAsyncIteration:
                    mp3_piece = None  # 流结束

                if mp3_piece:
                    mp3_buffer.extend(mp3_piece)

                # 攒够一段 OR 流结束 → 解码推送
                buffer_ready = len(mp3_buffer) >= self.MP3_FLUSH_BYTES
                stream_done = mp3_piece is None

                if not (buffer_ready or stream_done):
                    continue

                if not mp3_buffer and not prev_tail:
                    break  # 整个流为空

                # 解码：prev_tail + 当前累积的 MP3
                decode_input = bytes(prev_tail) + bytes(mp3_buffer)
                try:
                    seg = AudioSegment.from_file(
                        io.BytesIO(decode_input), format="mp3"
                    )
                    seg = (
                        seg.set_frame_rate(DEFAULT_SAMPLE_RATE)
                        .set_channels(DEFAULT_CHANNELS)
                        .set_sample_width(DEFAULT_SAMPLE_WIDTH)
                    )
                except Exception as e:  # noqa: BLE001
                    log.error(f"[edge-tts] 增量解码失败: {e}")
                    mp3_buffer.clear()
                    if stream_done:
                        break
                    continue

                # 如果不是最后一段，砍掉末尾一点点（会进下一段的 prev_tail 重叠解码）
                raw = seg.raw_data
                if not stream_done and len(raw) > chunk_bytes:
                    # 保留末尾用于下一段重叠（按 PCM 字节数转 MP3 估计）
                    tail_pcm = raw[-chunk_bytes:]
                    # 推送除末尾段之外的所有完整段（末尾段留到下次重叠重解码，避免交界爆音）
                    pushable = raw[:-chunk_bytes]
                    if pushable:
                        for i in range(0, len(pushable), chunk_bytes):
                            yield pushable[i : i + chunk_bytes]
                    # 记录这次 MP3 末尾用于下次重叠
                    prev_tail = bytes(mp3_buffer)[-self.MP3_OVERLAP_BYTES:]
                    mp3_buffer.clear()
                else:
                    # 最后一段：全部推送
                    for i in range(0, len(raw), chunk_bytes):
                        yield raw[i : i + chunk_bytes]
                    mp3_buffer.clear()
                    prev_tail = b""

                if stream_done:
                    break
        finally:
            loop.close()


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  后端实现：PowerShell + System.Speech（本地 Windows SAPI，可选）             ║
# ╚══════════════════════════════════════════════════════════════════════════╝
class _PowershellTTSBackend:
    """本地 TTS：调用 Windows PowerShell 的 System.Speech 合成 WAV。

    与 UE 端 nav.tts 用的完全是同一套底层（.NET SpeechSynthesizer），
    所以音色一致、零网络往返、零模型下载。

    实现方式：每次合成启动一个独立 PowerShell 子进程（用 SetOutputToWaveFile
    输出到临时 WAV），我们再读取 WAV 转成统一 PCM。
    子进程隔离 → 完全避开 pyttsx3/comtypes 的 COM 线程死锁问题。

    仅 Windows 可用。非 Windows 平台 available=False，TTSEngine 会报错。
    """

    name = "local"
    is_local = True
    is_streaming_supported = True  # 合成完切片，体验等同流式

    def __init__(self):
        self._available: Optional[bool] = None

    @property
    def available(self) -> bool:
        if self._available is not None:
            return self._available
        if not IS_WINDOWS:
            self._available = False
            return False
        # 探测 powershell 是否存在
        try:
            subprocess.run(
                ["powershell", "-NoProfile", "-Command", "exit 0"],
                capture_output=True, timeout=5, check=False,
            )
            self._available = True
        except Exception:
            self._available = False
        return self._available

    def synthesize_pcm(self, text: str) -> bytes:
        """启动 PowerShell 子进程合成 WAV，读取并重采样为统一 PCM。失败返回 b''。"""
        if not self.available:
            return b""

        tmp = tempfile.NamedTemporaryFile(
            suffix=".wav", delete=False, prefix="tts_ps_"
        )
        tmp_path = tmp.name
        tmp.close()

        # PowerShell 脚本：单引号转义后嵌入
        escaped = text.replace("'", "''")
        # 注意：WAV 路径里的反斜杠在 PS 字符串里要转义，用 here-string 避免麻烦
        script = (
            "Add-Type -AssemblyName System.Speech;"
            "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
            f"$s.Rate={LOCAL_SAPI_RATE};"
            f"$s.SetOutputToWaveFile('{tmp_path}');"
            f"$s.Speak('{escaped}');"
            "$s.Dispose()"
        )

        try:
            result = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive",
                 "-WindowStyle", "Hidden", "-Command", script],
                capture_output=True, timeout=15, check=False,
            )
            if result.returncode != 0:
                log.error(
                    f"[local-tts] PowerShell 失败 rc={result.returncode}: "
                    f"{result.stderr.decode('utf-8', errors='ignore')[:200]}"
                )
                return b""

            if not os.path.exists(tmp_path) or os.path.getsize(tmp_path) < 44:
                log.error("[local-tts] WAV 未生成或过小")
                return b""

            with wave.open(tmp_path, "rb") as w:
                pcm = w.readframes(w.getnframes())
                src_rate = w.getframerate()
                src_width = w.getsampwidth()
                src_channels = w.getnchannels()

            if not pcm:
                return b""

            seg = AudioSegment(
                data=pcm,
                sample_width=src_width,
                frame_rate=src_rate,
                channels=src_channels,
            )
            seg = (
                seg.set_frame_rate(DEFAULT_SAMPLE_RATE)
                .set_channels(DEFAULT_CHANNELS)
                .set_sample_width(DEFAULT_SAMPLE_WIDTH)
            )
            return seg.raw_data
        except subprocess.TimeoutExpired:
            log.error("[local-tts] PowerShell 超时")
            return b""
        except Exception as e:  # noqa: BLE001
            log.error(f"[local-tts] 合成失败: {e}")
            return b""
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    def stream_pcm_chunks(self, text: str) -> Iterator[bytes]:
        """合成完整 PCM 后按 ~200ms 切片流式产出。"""
        pcm = self.synthesize_pcm(text)
        if not pcm:
            return
        chunk_bytes = DEFAULT_SAMPLE_RATE * DEFAULT_SAMPLE_WIDTH * \
            DEFAULT_CHANNELS * STREAM_CHUNK_MS // 1000
        for i in range(0, len(pcm), chunk_bytes):
            yield pcm[i : i + chunk_bytes]


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  公共工具                                                                   ║
# ╚══════════════════════════════════════════════════════════════════════════╝
def _split_pcm(pcm: bytes, chunk_ms: int = STREAM_CHUNK_MS) -> Iterator[bytes]:
    """把原始 PCM 按 chunk_ms 切成等长段。"""
    if not pcm:
        return
    chunk_bytes = DEFAULT_SAMPLE_RATE * DEFAULT_SAMPLE_WIDTH * \
        DEFAULT_CHANNELS * chunk_ms // 1000
    for i in range(0, len(pcm), chunk_bytes):
        yield pcm[i : i + chunk_bytes]


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  统一入口 TTSEngine                                                         ║
# ╚══════════════════════════════════════════════════════════════════════════╝
class TTSEngine:
    """
    统一 TTS 入口。唯一后端为云端 edge-tts（无回退、无环境变量切换）。

    构造时不需要网络初始化（edge-tts 在合成时才连云），所以构造总是成功；
    但 local 后端要求 Windows + PowerShell 可用。
    """

    def __init__(self, backend: str = DEFAULT_BACKEND):
        backend = (backend or DEFAULT_BACKEND).lower()
        if backend == "local":
            self._backend = _PowershellTTSBackend()
            if not self._backend.available:
                raise RuntimeError(
                    "本地 TTS 后端不可用：需要 Windows + PowerShell。"
                    "可改环境变量 TTS_BACKEND=edge 用云端后端。"
                )
            log.info("TTS backend: local (PowerShell System.Speech)")
        else:
            self._backend = _EdgeTTSBackend()
            log.info(
                f"TTS backend: edge-tts (云端, voice={DEFAULT_VOICE})"
            )

    # ── 对外属性 ──────────────────────────────────────────────────────────
    @property
    def backend_name(self) -> str:
        return self._backend.name

    @property
    def is_local(self) -> bool:
        return self._backend.is_local

    @property
    def is_streaming_supported(self) -> bool:
        return self._backend.is_streaming_supported

    @property
    def sample_rate(self) -> int:
        return DEFAULT_SAMPLE_RATE

    @property
    def sample_width(self) -> int:
        return DEFAULT_SAMPLE_WIDTH

    @property
    def channels(self) -> int:
        return DEFAULT_CHANNELS

    @property
    def cache_size(self) -> int:
        return _cache.size

    # ── 核心 API ──────────────────────────────────────────────────────────
    def stream_pcm_chunks(self, text: str) -> "Iterator[tuple[bytes, bool]]":
        """流式产出 PCM 段。

        Yields:
            (pcm_bytes, cached) — cached 表示这一段是否来自缓存
            （缓存命中时只有一段完整 PCM 且 cached=True）。
        """
        text = (text or "").strip()
        if not text:
            return

        # 缓存命中：直接整段返回
        if CACHE_ENABLED:
            cached = _cache.get(text)
            if cached is not None:
                log.info(f"[tts] cache HIT ({len(cached)}B) text={text[:30]!r}")
                yield cached, True
                return

        # 缓存未命中：消费后端的增量流式产出，边 yield 边累积（用于落缓存）
        collected = bytearray()
        produced_any = False
        for chunk in self._backend.stream_pcm_chunks(text):
            collected.extend(chunk)
            produced_any = True
            yield chunk, False

        if produced_any and CACHE_ENABLED:
            _cache.put(text, bytes(collected))
            log.info(
                f"[tts] cache PUT ({len(collected)}B) "
                f"text={text[:30]!r} total={_cache.size}"
            )

    def generate_pcm(self, text: str) -> bytes:
        """一次性返回完整 PCM。命中缓存时延迟 < 5ms。"""
        text = (text or "").strip()
        if not text:
            return b""
        if CACHE_ENABLED:
            cached = _cache.get(text)
            if cached is not None:
                return cached
        return b"".join(c for c, _ in self.stream_pcm_chunks(text))

    # ── 高层封装：直接产出可下发 WebSocket 的消息序列 ─────────────────────
    def build_ws_stream_messages(self, text: str,
                                 timestamp: int) -> "Iterator[dict]":
        """产出可直接通过 WS 推送的消息字典序列（带 base64 音频）。

        消息协议见模块顶部文档字符串。
        """
        text = (text or "").strip()
        if not text:
            return

        # start
        yield {
            "type": "nav_audio_start",
            "text": text,
            "timestamp": timestamp,
            "sample_rate": self.sample_rate,
            "sample_width": self.sample_width,
            "channels": self.channels,
            "backend": self.backend_name,
        }

        seq = 0
        any_chunk = False
        cached_hit = False
        for pcm, cached in self.stream_pcm_chunks(text):
            any_chunk = True
            cached_hit = cached_hit or cached
            yield {
                "type": "nav_audio_chunk",
                "seq": seq,
                "audio": base64.b64encode(pcm).decode("ascii"),
                "timestamp": timestamp,
                "cached": cached,
            }
            seq += 1

        if not any_chunk:
            # 没有产出（合成失败）→ 回退为纯文本提示
            yield {"type": "nav_prompt", "text": text, "timestamp": timestamp}
            return

        yield {
            "type": "nav_audio_end",
            "timestamp": timestamp,
            "backend": self.backend_name,
            "cached": cached_hit,
            "chunks": seq,
        }


# ╔══════════════════════════════════════════════════════════════════════════╗
# ║  模块级单例（懒加载）                                                       ║
# ╚══════════════════════════════════════════════════════════════════════════╝
_engine_instance: Optional[TTSEngine] = None
_engine_lock = threading.Lock()


def get_engine(backend: Optional[str] = None) -> TTSEngine:
    """获取全局 TTSEngine 单例。

    backend:
        None → 用环境变量 TTS_BACKEND（未设则 "edge"）
        "edge"  → 云端 edge-tts
        "local" → 本地 PowerShell System.Speech
    首次调用后忽略 backend 参数（保持单例）；切换需重启或 reset_engine()。
    """
    global _engine_instance
    if _engine_instance is None:
        with _engine_lock:
            if _engine_instance is None:
                _engine_instance = TTSEngine(backend=backend or DEFAULT_BACKEND)
    return _engine_instance


def reset_engine():
    """重置单例（主要用于测试）。"""
    global _engine_instance
    with _engine_lock:
        _engine_instance = None
