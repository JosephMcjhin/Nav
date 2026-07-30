#include "PlanState.h"

#include "../NavigationComponent.h"

void FPlanState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  Nav.StopDrip();
  if (Nav.CurrentWaypointIndex < 0) {
    Nav.ReplanFromDeviation(Ctx);
  }
}

void FPlanState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  // Plan 状态下再次偏离 → SwitchTo(Plan) 同状态跳过 OnEnter → Tick 兜底
  if (Nav.CurrentWaypointIndex < 0) {
    Nav.ReplanFromDeviation(Ctx);
  }
}
