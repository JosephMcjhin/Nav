// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace Project001Console {
bool IsLocalNavTTSEnabled();
void SetLocalNavTTSEnabled(bool bEnabled);
void SpeakLocalNavText(const FString &Text);

// 返回当前二进制的构建时间戳（编译期生成，格式 "YYYY-MM-DD HH:MM:SS"）。
// 用于打包后版本确认：同一份打包出来的二进制，这个字符串是固定的；
// 重新编译/打包会得到新的时间戳。
const FString &GetBuildTimestamp();

// ── 本地蜂鸣播放（debug 调试用，与眼镜端 BeepPlayer 同等效果的柔和立体声） ──
// 由 SendBeepCommand 在 IsLocalNavTTSEnabled() 时调用，替代旧的 Windows ::Beep()。
//   - bActive=false：停止蜂鸣
//   - bActive=true, FreqHz>0：播放，FreqHz 频率（Hz），Pan 左右平衡（-1..1），Volume 0..1
// 仅在 PLATFORM_WINDOWS 下有效；其他平台为空实现。
void PlayLocalBeep(bool bActive, int32 FreqHz, float Pan = 0.0f,
                   float Volume = 1.0f, float IntervalMs = 0.0f);
}
