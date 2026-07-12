#include "PlanState.h"

#include "NavigationSystem.h"
#include "../NavigationComponent.h"

void FPlanState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  

  const bool bDeviation = (Nav.CurrentWaypointIndex < 0);

  if (bDeviation) {
    // ── 偏离：全局重新寻路 + 锁路点 ─────────────────────────
    if (Nav.CachedNavSys && Nav.ActiveTarget != NAME_None) {
      UNavigationPath* Path =
          Nav.CachedNavSys->FindPathToLocationSynchronously(
              Nav.GetWorld(), Ctx.PlayerLoc, Nav.ActiveTargetLocation);
      if (Path && Path->PathPoints.Num() > 0) {
        Nav.PlannedWaypoints = Path->PathPoints;
        Nav.CurrentWaypointIndex = 1;
        Nav.LastReplanCheckTime = Ctx.CurrentTime;
      }
    }
  }
}

void FPlanState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  const float AbsErr = FMath::Abs(Ctx.AngleError);
  if (AbsErr < Nav.AlignToleranceDegrees * 0.5f) {
    
  }
  
}
