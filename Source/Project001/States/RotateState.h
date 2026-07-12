#pragma once

#include "CoreMinimal.h"
#include "../NavigationStateMachine.h"

// Rotate 状态：朝向校准。
class FRotateState : public FNavState {
 public:
  void OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) override;
  void OnExit(UNavigationComponent& Nav, FNavContext& Ctx) override;
  void Tick(UNavigationComponent& Nav, FNavContext& Ctx) override;
  FName GetName() const override { return FName(TEXT("ROTATE")); }

 private:
  // 状态内部计时
  float StartAngleError = 0.0f;     // 进入状态时的角度误差（不变）
  float LastHeadingSampleTime = 0.0f;
  float LastHeadingDegrees = 0.0f;
  float LastRepromptTime = 0.0f;
  float LastAnnouncedAngle = 999.0f;  // 已提示的最小角度阈值
  float PrevAbsError = 180.0f;       // 上一帧的角度误差绝对值
  bool bBeepStarted = false;  // 蜂鸣已启动，启动后不再停止
};
