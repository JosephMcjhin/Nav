# ATK-MS901M 十轴传感器模块 ↔ voice_nav 后端对接文档

> 参考文档：`ATK-MS901M模块使用说明_V1.2.pdf`
> 对接后端：`MobileServer/voice_nav/web_app.py`

---

## 1. 模块概述（来自 PDF）

ATK-MS901M 是正点原子的**十轴姿态传感器**（陀螺仪+加速度计+磁力计+气压计），通过 **UART 主动上报**数据，无需主机轮询。

| 数据帧 ID 宏 | 含义 | 数据格式 |
|---|---|---|
| `ATK_MS901M_FRAME_ID_ATTITUDE` | 姿态角 roll/pitch/yaw | int16×3，缩放 `/32768*180` → 度 |
| `ATK_MS901M_FRAME_ID_GYRO_ACCE` | 陀螺仪+加速度 | int16×6，按满量程缩放 |
| `ATK_MS901M_FRAME_ID_MAG` | 磁力计+温度 | int16×3 + int16/100 |
| `ATK_MS901M_FRAME_ID_BARO` | 气压+高度+温度 | int32×2 + int16/100 |

**UART 帧格式**：`[HEAD_L][HEAD_H][ID][LEN][DAT...][SUM]`
- `SUM` = 前面所有字节累加和的低 8 位
- 波特率默认 115200

**关键换算（PDF 第 9 页 atk_ms901m_get_attitude）**：

```c
yaw   = (int16_t)(dat[5]<<8 | dat[4]) / 32768.0 * 180.0;   // 度，范围 ±180
pitch = (int16_t)(dat[3]<<8 | dat[2]) / 32768.0 * 180.0;
roll  = (int16_t)(dat[1]<<8 | dat[0]) / 32768.0 * 180.0;
```

> ⚠️ yaw 范围是 **±180°**（不是 0~360°），向左转为负，向右转为正。

---

## 2. 数据链路

```
ATK-MS901M ──UART 115200──> 网关(STM32/ESP32/PC) ──解析姿态帧──> JSON
                                                                          │
                                              ┌───────────────────────────┴──┐
                                              ▼ UDP :9003                     ▼ WS :8090/ws
                                       _udp_uwb_listener               WebSocket handler
                                       (web_app.py)                    msg_type="imu"
                                              │                           │
                                              └─────────┬─────────────────┘
                                                        ▼
                                  uwb_calibrator.update_imu_yaw(raw_yaw)
                                                        │
                                  uwb_calibrator.apply_imu_offset(raw_yaw)
                                                        │
                                                        ▼
                                  send_to_ue({"type":"set_rotation","yaw":...})
```

---

## 3. 网关需要做什么（参考实现）

网关任务：读 UART → 解析 ATTITUDE 帧 → 把 yaw 转成 JSON → UDP/WS 发到后端。

### 方式 A：UDP（推荐，最低延迟）

```python
# gateway_ms901m.py （PC/Linux/树莓派都可，pyserial）
import socket, struct, time, serial

SERIAL_PORT = "COM3"          # 改成你的串口
SERIAL_BAUD = 115200
SERVER_IP   = "192.168.1.100" # web_app.py 所在机器
SERVER_PORT = 9003

FRAME_HEAD_L = 0x5A
FRAME_HEAD_H = 0xA5
FRAME_ID_ATTITUDE = 0x01      # 替换成 atk_ms901m.h 里的真实宏值

ser = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=1)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def parse_attitude(dat: bytes):
    # 小端 int16 × 3
    roll, pitch, yaw = struct.unpack("<hhh", dat[:6])
    return roll/32768*180, pitch/32768*180, yaw/32768*180

buf = bytearray()
while True:
    chunk = ser.read(64)
    if not chunk:
        continue
    buf.extend(chunk)

    # 状态机找帧
    while len(buf) >= 4:
        # 找帧头
        i = buf.find(bytes([FRAME_HEAD_L, FRAME_HEAD_H]))
        if i < 0:
            buf.clear()
            break
        if i > 0:
            del buf[:i]
        if len(buf) < 4:
            break
        frame_id = buf[2]
        data_len = buf[3]
        total_len = 4 + data_len + 1   # header(4) + dat + sum(1)
        if len(buf) < total_len:
            break

        frame = buf[:total_len]
        checksum = sum(frame[:-1]) & 0xFF
        if checksum != frame[-1]:
            del buf[:1]                # 校验失败，跳一字节重新同步
            continue

        if frame_id == (0x80 | FRAME_ID_ATTITUDE):  # 上行帧 ID 高位置 1
            roll, pitch, yaw = parse_attitude(frame[4:4+data_len])
            payload = {
                "id": "MS901M_01",
                "ts": int(time.time()*1000),
                "freq": 50,
                "euler": [yaw, pitch, roll],
            }
            sock.sendto(
                (b'{"id":"MS901M_01","ts":%d,"freq":50,"euler":[%.3f,%.3f,%.3f]}'
                 % (payload["ts"], yaw, pitch, roll)),
                (SERVER_IP, SERVER_PORT),
            )
        del buf[:total_len]
```

### 方式 B：WebSocket

```python
# 用 websockets 库
import asyncio, json, websockets, serial, struct

async def push_imu():
    async with websockets.connect("ws://192.168.1.100:8090/ws") as ws:
        # ... 同上的 UART 解析 ...
        while True:
            roll, pitch, yaw = parse_attitude(...)
            await ws.send(json.dumps({"type": "imu", "yaw": yaw}))
            await asyncio.sleep(0.02)
```

---

## 4. 后端处理逻辑（已就绪，无需改动）

[`web_app.py`](web_app.py) 已经支持两条入口：

**UDP 入口** ([`_udp_uwb_listener`](web_app.py), 默认端口 9003):
```python
if "euler" in payload:
    raw_yaw = float(euler[0])
    uwb_calibrator.update_imu_yaw(raw_yaw)
    ...
    corrected_yaw = uwb_calibrator.apply_imu_offset(raw_yaw)
    send_to_ue({"type": "set_rotation", "yaw": corrected_yaw})
```

**WebSocket 入口** ([web_app.py:371](web_app.py)):
```python
elif msg_type == "imu":
    raw_yaw = float(msg.get("yaw", 0.0))
    uwb_calibrator.update_imu_yaw(raw_yaw)
    corrected_yaw = uwb_calibrator.apply_imu_offset(raw_yaw)
    send_to_ue({"type": "set_rotation", "yaw": corrected_yaw})
```

---

## 5. ⚠️ 关键注意事项

### 5.1 yaw 范围不匹配（最容易出 bug）

| 来源 | yaw 范围 |
|---|---|
| ATK-MS901M 原始输出 | **±180°** (左负右正) |
| `test_server.py` 仿真 | 0~360° (`% 360.0`) |
| `UwbCalibrationManager.apply_imu_offset` | 输出 0~360° (`% 360.0`) |
| Unreal Engine `set_rotation` | 通常 0~360° 或 ±180° 都接受 |

**建议**：在网关侧统一转换成 **0~360°** 再发给后端：

```python
yaw_0_360 = yaw % 360.0   # -170 → 190, 90 → 90
```

这样 `apply_imu_offset` 的 `% 360.0` 运算行为完全一致，校准和仿真数据行为统一。

### 5.2 UWB + IMU 必须都校准后才会推 UE

[`web_app.py`](web_app.py) 里这段：

```python
if (not uwb_calibrator.is_imu_calibrated or
        uwb_calibrator.transform_matrix is None):
    continue
```

意味着：**必须先完成 UWB 3 点标定 + IMU 朝向标定**，IMU 数据才会下发到 UE。校准流程：

1. UE 端按 `calibrate_point` 采集 3 个点 → `calibrate_solve` 解算仿射矩阵
2. 把眼镜朝向某个已知 UE 朝向（比如正北 = UE yaw 0）→ 发 `calibrate_heading` `{"target_ue_yaw": 0}`
3. 之后 ATK-MS901M 的 yaw 会自动 `apply_imu_offset` 后下发

### 5.3 ATK-MS901M 的 0° 方向

ATK-MS901M 上电时的 yaw=0 方向**取决于磁力计标定/上电姿态**。建议：
- 模块上电后让用户**面朝 UE 世界的 +Y 或 +X 方向**（你定的"正前方"）
- 此时记下的 raw_yaw 作为 `calibrate_heading` 的 IMU 输入
- 这样 `imu_offset` 就是把模块的 0° 对齐到 UE 的 0°

### 5.4 磁力计容易受干扰

PDF 中 ATK-MS901M 用磁力计融合得到绝对 yaw，**AR 眼镜/铁磁物体附近会有漂移**。建议：
- 模块远离电机/电池/钢铁结构 > 20cm
- 定期重新 `calibrate_heading`

---

## 6. 测试步骤

1. 启动后端：`web_app.py`（监听 UDP 9003 + WS 8090）
2. 启动 UE，确认有 UE WebSocket 客户端连上后端
3. **先用 `test_server.py`** 跑仿真，确认 UWB 3 点标定 + IMU 标定 + 循环行走能正常
4. 把 `test_server.py` 换成真实网关脚本（上面方式 A 或 B）
5. 检查 UE 里角色旋转方向是否符合物理方向（不一致时改 `apply_imu_offset` 的符号，或在网关侧 `yaw = -yaw`）

---

## 7. 可选：上传更多 ATK-MS901M 数据

如果以后想用加速度计做步态检测、或气压计做楼层判断，可在 JSON 里扩展：

```json
{
  "id": "MS901M_01",
  "ts": 1719000000000,
  "euler": [yaw, pitch, roll],
  "accel": [ax, ay, az],      // m/s²
  "gyro":  [gx, gy, gz],      // °/s
  "baro":  {"pressure": 101325, "altitude": 12.3, "temp": 25.6}
}
```

然后在 `_udp_uwb_listener` 里加 `elif "accel" in payload:` 等分支即可。目前后端只消费 `euler[0]`。
