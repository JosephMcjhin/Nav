# Project001 - Voice Navigation & UWB Tracking

本项目包含基于 Unreal Engine 5 的客户端及基于 Python 的导航/语音处理服务器。

## 📁 项目结构
- **Source/**: UE C++ 源码。
- **MobileServer/voice_nav/**: Python 语音处理、Whisper 识别与 UWB 追踪服务器。
- **MobileServer/ar_sensor/**: 基于手机浏览器的 AR 惯导信号转发服务。
- **Project001.uproject**: UE 项目描述文件。

## 🛠️ 环境要求
- **Unreal Engine**: 5.3 或更高。
- **Python**: **3.12+ (推荐 3.14)**。
  - 需要安装 [FFmpeg](https://ffmpeg.org/download.html) 并在环境变量中生效。
  - 依赖项见 `MobileServer/voice_nav/requirements.txt`。

## 🚀 快速启动
### 服务器 (PC端)
1. 进入目录：`cd MobileServer/voice_nav`
2. **首次运行**: 推荐安装 [uv](https://github.com/astral-sh/uv) 并执行 `uv pip install -r requirements.txt`。
3. **一键启动**: 直接运行 `start_server.bat` 即可启动 WebSocket 服务（默认端口 8090）。

### 客户端 (Unreal Engine)
- 在编辑器内运行，通过 `BeaconCalibrationWidget` 进行三点标定。

## 📱 手机端连接指引
1. 确保手机与 PC 处于**同一局域网**。
2. 启动服务器后，日志中会显示 `ws://[IP]:8090/ws`。
3. 在手机端 App 或浏览器中输入显示的 IP 地址进行连接。
   - *提示：如果连接失败，请检查 PC 防火墙是否允许 8090 和 9003 端口的入站连接。*

---
*注：本项目 Git 仓库当前不包含大型 Content 资源。*
