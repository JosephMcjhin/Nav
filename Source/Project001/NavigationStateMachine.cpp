#include "NavigationStateMachine.h"

#include "States/PlanState.h"
#include "States/RotateState.h"
#include "States/MoveState.h"
#include "NavigationComponent.h"

FNavStateMachine::FNavStateMachine() {
  States[static_cast<uint8>(ENavState::None)] = nullptr;
  States[static_cast<uint8>(ENavState::Plan)] = MakeUnique<FPlanState>();
  States[static_cast<uint8>(ENavState::Rotate)] = MakeUnique<FRotateState>();
  States[static_cast<uint8>(ENavState::Move)] = MakeUnique<FMoveState>();
}

void FNavStateMachine::Reset() {
  CurrentState = ENavState::None;
  LastWaypointIndex = -2;
}

void FNavStateMachine::SwitchTo(ENavState NewState, UNavigationComponent& Nav,
                                FNavContext& Ctx) {
  if (NewState == CurrentState) return;
  if (NewState == ENavState::None) {
    if (CurrentState != ENavState::None) {
      if (auto* S = States[static_cast<uint8>(CurrentState)].Get())
        S->OnExit(Nav, Ctx);
    }
    CurrentState = ENavState::None;
    return;
  }

  if (CurrentState != ENavState::None) {
    if (auto* S = States[static_cast<uint8>(CurrentState)].Get())
      S->OnExit(Nav, Ctx);
  }

  CurrentState = NewState;
  if (auto* S = States[static_cast<uint8>(CurrentState)].Get())
    S->OnEnter(Nav, Ctx);
}

ENavState FNavStateMachine::EvaluateState(UNavigationComponent& Nav,
                                          FNavContext& Ctx) {
  // 路点变化 → Plan
  if (Nav.CurrentWaypointIndex != LastWaypointIndex) {
    LastWaypointIndex = Nav.CurrentWaypointIndex;
    return ENavState::Plan;
  }

  // 未就绪
  if (Nav.CurrentWaypointIndex < 0 || Nav.PlannedWaypoints.Num() < 2) {
    return ENavState::None;
  }

  // 方向判断：Rotate ↔ Move
  const float AbsErr = FMath::Abs(Ctx.AngleError);
  if (CurrentState == ENavState::Rotate) {
    return AbsErr < Nav.AlignToleranceDegrees ? ENavState::Move
                                              : ENavState::Rotate;
  }
  // Move 状态：偏移过多 → Rotate
  if (AbsErr > Nav.ExecuteDriftDegrees) {
    return ENavState::Rotate;
  }
  return ENavState::Move;
}

void FNavStateMachine::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  // TTS 播放中 → 等待播完再评估
  if (Ctx.CurrentTime < Nav.NextPromptDispatchTime) {
    return;
  }

  // 计算段信息
  if (Nav.CurrentWaypointIndex >= 0 && Nav.PlannedWaypoints.Num() >= 2) {
    const FVector NextWaypoint =
        Nav.PlannedWaypoints[FMath::Min(Nav.CurrentWaypointIndex,
                                        Nav.PlannedWaypoints.Num() - 1)];
    Ctx.SegmentDir = (NextWaypoint - Ctx.PlayerLoc).GetSafeNormal();
    Ctx.RemainingMeters =
        FVector::Dist2D(Ctx.PlayerLoc, NextWaypoint) / 100.0f / Nav.DistanceScale;
    const float FDot = FVector::DotProduct(Ctx.PlayerForward, Ctx.SegmentDir);
    const float RDot = FVector::DotProduct(Ctx.PlayerRight, Ctx.SegmentDir);
    Ctx.AngleError = FMath::RadiansToDegrees(FMath::Atan2(RDot, FDot));
  }

  ENavState Desired = EvaluateState(Nav, Ctx);

  // 先 Tick 当前状态（让其有机会执行校准成功等效果），再切状态
  if (CurrentState != ENavState::None) {
    auto* S = States[static_cast<uint8>(CurrentState)].Get();
    if (S) S->Tick(Nav, Ctx);
  }

  if (Desired != CurrentState) {
    SwitchTo(Desired, Nav, Ctx);
  }
}
