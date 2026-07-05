#include "MoveState.h"

#include "../NavigationComponent.h"

void FMoveState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  Nav.bBeepActive = false;

  Ctx.StartLocation = Ctx.PlayerLoc;

  LastMoveSampleTime = Ctx.CurrentTime;
  LastTraveledMeters = 0.0f;
  LastRepromptTime = Ctx.CurrentTime;
  DistLastAnnounced = 999.0f;

  // TTS 距离提示
  FString Prompt = FString::Printf(TEXT("%s%.1f%s"),
                                    UTF8_TO_TCHAR(u8"请直走约"),
                                    Ctx.RemainingMeters,
                                    UTF8_TO_TCHAR(u8"米"));
  Nav.EnqueueHighPriorityPrompt(Prompt);
}

ENavState FMoveState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  const float AbsErr = FMath::Abs(Ctx.AngleError);

  // 转移：偏太多 → 回 Rotate
  if (AbsErr > Nav.ExecuteDriftDegrees) {
    return ENavState::Rotate;
  }

  const float Traveled = FVector::Dist2D(Ctx.PlayerLoc, Ctx.StartLocation) /
                         100.0f / Nav.DistanceScale;

  // 蜂鸣：走出去才响。
  if (Traveled >= Nav.ExecuteBeepStartMeters) {
    const float Total =
        FMath::Max(Traveled + Ctx.RemainingMeters, KINDA_SMALL_NUMBER);
    const float Progress =
        FMath::Clamp(1.0f - (Ctx.RemainingMeters / Total), 0.0f, 1.0f);
    const int32 FreqHz = FMath::RoundToInt(400.0f + 800.0f * Progress);
    Nav.SendBeepCommand(true, FreqHz);
  } else {
    if (Nav.bBeepActive) Nav.StopBeep();
  }

  // 静止检测
  const bool bMoved =
      FMath::Abs(Traveled - LastTraveledMeters) > 0.05f;
  if (bMoved) {
    LastMoveSampleTime = Ctx.CurrentTime;
    LastTraveledMeters = Traveled;
  }
  const bool bIdle = (Ctx.CurrentTime - LastMoveSampleTime) >
                     Nav.AlignIdleRepromptSeconds;
  if (bIdle && (Ctx.CurrentTime - LastRepromptTime >
                Nav.AlignIdleRepromptSeconds)) {
    LastRepromptTime = Ctx.CurrentTime;
    Nav.EnqueueMediumPriorityPrompt(
        FString::Printf(TEXT("%s%.1f%s"), UTF8_TO_TCHAR(u8"请直走约"),
                        Ctx.RemainingMeters, UTF8_TO_TCHAR(u8"米")));
  }

  // 接近提示：距离跨过阈值时播报
  {
    const float Thresholds[] = {2.0f, 1.0f, 0.5f};
    for (float T : Thresholds) {
      if (Ctx.RemainingMeters <= T && DistLastAnnounced > T) {
        DistLastAnnounced = T;
        Nav.EnqueueMediumPriorityPrompt(
            FString::Printf(TEXT("%.1f%s"),
                            Ctx.RemainingMeters,
                            UTF8_TO_TCHAR(u8"米")));
        break;
      }
    }
  }

  return ENavState::Move;  // 保持自己
}
