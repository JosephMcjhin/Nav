#include "RotateState.h"

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

void FRotateState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  Nav.bBeepActive = false;
  bBeepStarted = false;

  // 起始快照
  Ctx.StartLocation = Ctx.PlayerLoc;

  // 内部计时初始化
  LastHeadingSampleTime = Ctx.CurrentTime;
  LastHeadingDegrees = Ctx.AngleError;
  StartAngleError = Ctx.AngleError;   // 不变，用于计算转头量
  LastRepromptTime = Ctx.CurrentTime;
  LastAnnouncedAngle = 999.0f;

  // TTS 转向提示
  FString Prompt = BuildTurnPrompt(Ctx.AngleError);
  Nav.EnqueueHighPriorityPrompt(Prompt);
}

ENavState FRotateState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  const float AbsErr = FMath::Abs(Ctx.AngleError);

  // 转移：对准了 → Move
  if (AbsErr < Nav.AlignToleranceDegrees) {
    return ENavState::Move;
  }

  // 卡住检测
  const bool bMoving = FMath::Abs(FMath::FindDeltaAngleDegrees(
      LastHeadingDegrees, Ctx.AngleError)) > 3.0f;
  if (bMoving) {
    LastHeadingSampleTime = Ctx.CurrentTime;
    LastHeadingDegrees = Ctx.AngleError;
  }
  const bool bIdle = (Ctx.CurrentTime - LastHeadingSampleTime) >
                     Nav.AlignIdleRepromptSeconds;
  if (bIdle && (Ctx.CurrentTime - LastRepromptTime >
                Nav.AlignIdleRepromptSeconds)) {
    LastRepromptTime = Ctx.CurrentTime;
    Nav.EnqueueMediumPriorityPrompt(BuildTurnPrompt(Ctx.AngleError));
  }

  // 蜂鸣：用户开始转之后才响，一旦启动就不再停止。
  {
    const float TurnedFromStart = FMath::Abs(FMath::FindDeltaAngleDegrees(
        StartAngleError, Ctx.AngleError));
    if (!bBeepStarted) {
      bBeepStarted = (TurnedFromStart >= Nav.AlignBeepStartDeltaDegrees);
    }
    if (bBeepStarted) {
      const float Progress = FMath::Clamp(1.0f - (AbsErr / 90.0f), 0.0f, 1.0f);
      const int32 FreqHz = FMath::RoundToInt(400.0f + 800.0f * Progress);
      Nav.SendBeepCommand(true, FreqHz);
    }
  }

  // 接近提示：角度跨过阈值时播报
  {
    const float Thresholds[] = {30.0f, 10.0f};
    for (float T : Thresholds) {
      if (AbsErr <= T && LastAnnouncedAngle > T) {
        LastAnnouncedAngle = T;
        Nav.EnqueueMediumPriorityPrompt(
            FString::Printf(TEXT("%d%s"),
                            FMath::RoundToInt(AbsErr),
                            UTF8_TO_TCHAR(u8"度")));
        break;
      }
    }
  }

  return ENavState::Rotate;  // 保持自己
}