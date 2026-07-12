// LocalBeepPlayer：本地柔和立体声蜂鸣播放器（debug 调试用）
//
// 用 Windows waveOut API 流式播放 PCM，与 Android 端 BeepPlayer.kt 逻辑镜像：
//  - 正弦 + 弱二次/三次谐波 + 一阶低通滤波
//  - ADSR 包络（attack/release）消除爆破音
//  - 立体声等功率 pan 律
// 替代旧的 Windows ::Beep()（主板方波蜂鸣器，刺耳）。
//
// 仅 PLATFORM_WINDOWS 下有实际实现；其他平台为空操作。
#pragma once

#include "CoreMinimal.h"

/**
 * 本地柔和立体声蜂鸣播放器（单例）。
 *
 * 由 Project001Console::PlayLocalBeep 调用，在 UE 编辑器 debug 时
 * 提供与眼镜端 BeepPlayer.kt 一致的听感。
 *
 * 所有实现细节（waveOut handle、PCM 缓冲、Runnable 线程）都藏在 .cpp 中。
 */
class PROJECT001_API FLocalBeepPlayer {
 public:
  /** 获取单例。 */
  static FLocalBeepPlayer& Get();

  /**
   * 主接口。
   *   bActive=false → 停止
   *   bActive=true, FreqHz>0 → 启动/更新参数
   *   - Pan: -1=全左, 0=居中, +1=全右
   *   - Volume: 0..1 总音量缩放
   */
  void SetActive(bool bActive, int32 FreqHz, float Pan, float Volume, float IntervalMs);

  /** 停止蜂鸣并释放底层资源。 */
  void Stop();

  ~FLocalBeepPlayer();

 private:
  FLocalBeepPlayer();
  // 实现状态藏在 .cpp 的 FLocalBeepPlayerImpl 中，头文件只保留空壳。
  struct FImpl;
  FImpl* Impl = nullptr;
};
