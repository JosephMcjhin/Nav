#pragma once

#include "CoreMinimal.h"
#include "../NavigationStateMachine.h"

// Move 状态：直走（原 Execute）。
class FMoveState : public FNavState {
 public:
  void OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) override;
  ENavState Tick(UNavigationComponent& Nav, FNavContext& Ctx) override;
  FName GetName() const override { return FName(TEXT("MOVE")); }

 private:
  // 状态内部计时
  float LastMoveSampleTime = 0.0f;
  float LastTraveledMeters = 0.0f;
  float LastRepromptTime = 0.0f;
  float DistLastAnnounced = 999.0f;  // 已提示的最小距离阈值
};
