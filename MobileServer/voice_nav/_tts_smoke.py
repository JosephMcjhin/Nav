import time
from modules.tts_engine import get_engine

t0 = time.time()
eng = get_engine()
print(f"init OK in {(time.time()-t0)*1000:.0f}ms, backend={eng.backend_name}")

# 合成 + WS 消息（真实网络调用）
t0 = time.time()
msgs = list(eng.build_ws_stream_messages("前方五米左转,到达工位", 123456))
dt = (time.time() - t0) * 1000
types = [m["type"] for m in msgs]
n_chunks = sum(1 for m in msgs if m["type"] == "nav_audio_chunk")
print(f"synth: {dt:.0f}ms, msg_types={types[0]}..{types[-1]}, chunks={n_chunks}")

# 第二次（应命中缓存，不走网络）
t0 = time.time()
msgs2 = list(eng.build_ws_stream_messages("前方五米左转,到达工位", 123457))
print(f"2nd call (cached): {(time.time()-t0)*1000:.1f}ms, cached={msgs2[1]['cached']}")
