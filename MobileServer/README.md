# MobileServer

统一移动端服务后端，包含两个独立子服务：
1. `ar_sensor/`: 手机传感器中继（控制 UE 角色移动，提供实时 Yaw 和步频）
2. `voice_nav/`: 语音导航控制核心

---

## 语音导航 (voice_nav)

底层使用了你的增强版 `speech_to_nav.py`（基于 OpenAI Whisper），在此之上我们添加了 `web_app.py` 提供手机浏览器的录音功能。完全兼容你的原始设定与指令！

### 安装依赖

```powershell
cd MobileServer/voice_nav
uv pip install --system -r requirements.txt
```

### 启动组合模式

**模式A：仅使用 PC 麦克风**
(和以前完全一样)
```powershell
python speech_to_nav.py
```

**模式B：仅使用手机端录音**
(让你能在走动时拿手机控制)
```powershell
python web_app.py --host 0.0.0.0 --port 8090
```
> 然后手机连接同一局域网并访问：`http://<PC-IP>:8090`，即可按住按钮说话导航。

**模式C：PC + 手机端 同时启用！**
```powershell
python web_app.py --host 0.0.0.0 --port 8090 --with-pc
```
> 这样无论你对着 PC 麦克风喊，还是对着手机网页喊，UE 都能接收到导航指令，并且语音播报（TTS）在 PC 和手机端会同时响起。

### 作为 AI Agent 工具 (MCP)
(完全保留你的扩展)
```powershell
python nav_mcp_server.py
```
可以在 Claude Desktop / Cursor 中直接配置为 MCP 服务器。
