#include "MoveState.h"

#include "../NavigationComponent.h"

void FMoveState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  Nav.StopDrip();

  Ctx.StartLocation = Ctx.PlayerLoc;

  LastMoveSampleTime = Ctx.CurrentTime;
  LastTraveledMeters = 0.0f;
  LastRepromptTime = Ctx.CurrentTime;

  // TTS 距离提示
  FString Prompt = FString::Printf(TEXT("%s%.1f%s"),
                                    UTF8_TO_TCHAR(u8"请直走约"),
                                    Ctx.RemainingMeters,
                                    UTF8_TO_TCHAR(u8"米"));
  Nav.EnqueuePrompt(Prompt);
}

void FMoveState::OnExit(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopDrip();
}

void FMoveState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  // 注：状态转移（含偏移过多切回 Rotate）由
  // FNavStateMachine::EvaluateState 统一管理，此处 Tick 的返回值会被忽略。

  const float AbsErr = FMath::Abs(Ctx.AngleError);

  if (AbsErr > Nav.ExecuteDriftDegrees) {
    Nav.ClearNonCriticalPrompts();
    Nav.SendSoundEffect(ENavSoundCategory::SFX_Deviation);
  }

  const float Traveled = FVector::Dist2D(Ctx.PlayerLoc, Ctx.StartLocation) /
                         100.0f / Nav.DistanceScale;

  // 水滴引导：用统一间隔（BeepUpdateIntervalSeconds），发送一次消息给客户端自行循环。
  if (Traveled >= Nav.ExecuteBeepStartMeters) {
    const float Total =
        FMath::Max(Traveled + Ctx.RemainingMeters, KINDA_SMALL_NUMBER);
    const float Progress =
        FMath::Clamp(1.0f - (Ctx.RemainingMeters / Total), 0.0f, 1.0f);
    // 越接近目标，间隔越短（与 beep 统一公式）
    if (Nav.SoundComp) {
      Nav.SoundComp->BeepUpdateIntervalSeconds =
          FMath::Lerp(1.0f, 0.2f, Progress * Progress);
      Nav.SendDripCommand(true,
                          Nav.SoundComp->BeepUpdateIntervalSeconds * 1000.0f);
    }
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
    Nav.EnqueuePrompt(
        FString::Printf(TEXT("%s%.1f%s"), UTF8_TO_TCHAR(u8"请直走约"),
                        Ctx.RemainingMeters, UTF8_TO_TCHAR(u8"米")));
  }

  // 接近提示：仅当距离收窄跨过阈值时播报
  {
    const float Thresholds[] = {2.0f, 1.0f, 0.5f};
    for (float T : Thresholds) {
      if (Ctx.RemainingMeters <= T && PrevRemainingMeters > T) {
        Nav.EnqueuePrompt(
            FString::Printf(TEXT("%.1f%s"), T,
                            UTF8_TO_TCHAR(u8"米")));
        break;
      }
    }
  }

  PrevRemainingMeters = Ctx.RemainingMeters;

    // 保持自己
}
