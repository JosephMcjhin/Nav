# Project001 - Voice Navigation & UWB Tracking

本项目包含基于 Unreal Engine 5 的客户端及基于 Python 的导航/语音处理服务器。

## 项目结构
- **Source/**: UE C++ 源码。
- **Config/**: 项目配置文件。
- **MobileServer/voice_nav/**: Python 语音处理与 UWB 追踪服务器。
- **Project001.uproject**: UE 项目描述文件。

## 环境要求
- **Unreal Engine**: 5.x (建议 5.3 或更高)。
- **Python**: 3.9+ 
  - 依赖项见 `MobileServer/voice_nav/requirements.txt`。

## 快速启动 (服务器)
1. 进入服务器目录: `cd MobileServer/voice_nav`
2. 安装依赖: `pip install -r requirements.txt`
3. 启动应用: `python web_app.py --port 8090`

## 标定与导航说明
1. 在 UE 中打开 `BeaconCalibrationWidget` 进行两点标定。
2. 标定完成后，角色将自动跟踪 UWB 模拟器 (`test_uwb_sim.py`) 或真实硬件的位移。

---
*注：本项目 Git 仓库当前不包含 Content 资源。*
