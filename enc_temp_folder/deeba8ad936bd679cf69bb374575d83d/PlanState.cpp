#include "PlanState.h"

#include "../NavigationMathLibrary.h"
#include "NavigationSystem.h"
#include "../NavigationComponent.h"

namespace {
FString BuildTurnPrompt(float SignedAngleDegrees) {
  const float AbsDeg = FMath::Abs(SignedAngleDegrees);
  const int32 IntDeg = FMath::RoundToInt(AbsDeg);
  if (AbsDeg < 8.0f) {
    return UTF8_TO_TCHAR(u8"已经对准，请直走");
  }
  const FString Dir = (SignedAngleDegrees >= 0.0f) ? UTF8_TO_TCHAR(u8"右")
                                                   : UTF8_TO_TCHAR(u8"左");
  return FString::Printf(TEXT("%s%s%d%s"), UTF8_TO_TCHAR(u8"请向"), *Dir,
                         IntDeg, UTF8_TO_TCHAR(u8"度转身"));
}
}  // namespace

void FPlanState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  Nav.bBeepActive = false;

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

ENavState FPlanState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  const float AbsErr = FMath::Abs(Ctx.AngleError);
  if (AbsErr < Nav.AlignToleranceDegrees * 0.5f) {
    return ENavState::Move;
  }
  return ENavState::Rotate;
}
