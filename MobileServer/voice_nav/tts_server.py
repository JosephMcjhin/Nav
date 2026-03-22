import socket
import os
import tempfile
import asyncio
import edge_tts
import ctypes

UDP_IP = "127.0.0.1"
UDP_PORT = 9002

class TTSServerProtocol(asyncio.DatagramProtocol):
    def __init__(self):
        super().__init__()
        self.current_task = None
        self.play_alias = "tts_player"
        self.last_temp_file = None

    def connection_made(self, transport):
        self.transport = transport
        print(f"[TTS] Ready and listening on UDP {UDP_IP}:{UDP_PORT} (Running Async Edge-TTS)")

    def datagram_received(self, data, addr):
        text = data.decode("utf-8").strip()
        if text:
            # Cancel any currently generating or playing TTS
            if self.current_task and not self.current_task.done():
                self.current_task.cancel()
            
            # Immediately tell Windows to stop the current audio
            ctypes.windll.winmm.mciSendStringW(f'close {self.play_alias}', None, 0, None)
            
            # Clean up the previous temp file if it exists
            if self.last_temp_file and os.path.exists(self.last_temp_file):
                try:
                    os.remove(self.last_temp_file)
                except OSError:
                    pass
            
            self.current_task = asyncio.create_task(self.process_tts(text))

    async def process_tts(self, text):
        temp_file = tempfile.NamedTemporaryFile(suffix=".mp3", delete=False)
        temp_file_path = temp_file.name
        temp_file.close()
        self.last_temp_file = temp_file_path

        try:
            print(f"[TTS] Generating: '{text}'")
            communicate = edge_tts.Communicate(text, "zh-CN-XiaoxiaoNeural")
            await communicate.save(temp_file_path)
            
            print(f"[TTS] Playing...")
            # Use Windows MCI framework for robust background playback
            ctypes.windll.winmm.mciSendStringW(f'open "{temp_file_path}" alias {self.play_alias}', None, 0, None)
            ctypes.windll.winmm.mciSendStringW(f'play {self.play_alias}', None, 0, None)
            
            # Poll until playback finishes
            buf = ctypes.create_unicode_buffer(256)
            while True:
                await asyncio.sleep(0.5)
                ctypes.windll.winmm.mciSendStringW(f'status {self.play_alias} mode', buf, 255, None)
                if buf.value != "playing":
                    break
                    
            # Once done, close the handle so we can delete the file later
            ctypes.windll.winmm.mciSendStringW(f'close {self.play_alias}', None, 0, None)

        except asyncio.CancelledError:
            # Task was cancelled by a new incoming UDP sentence
            print("[TTS] Interrupted by new message.")
        except Exception as e:
            print(f"[TTS] Play error: {e}")

async def main():
    loop = asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: TTSServerProtocol(),
        local_addr=(UDP_IP, UDP_PORT)
    )
    try:
        await asyncio.sleep(3600*24)
    finally:
        transport.close()

if __name__ == "__main__":
    asyncio.run(main())
