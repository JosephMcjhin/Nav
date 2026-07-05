#pragma once

#include "CoreMinimal.h"
#include "../NavigationStateMachine.h"

// Plan 状态：
// - index == -1（偏离）→ 全局重新寻路 + 锁路点 + 播总览
// - index >= 1（段切换）→ 播报下一段信息（转向/直走）
// 等说完后根据朝向决定 Rotate 或 Move。
class FPlanState : public FNavState {
 public:
  void OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) override;
  ENavState Tick(UNavigationComponent& Nav, FNavContext& Ctx) override;
  FName GetName() const override { return FName(TEXT("PLAN")); }
};
