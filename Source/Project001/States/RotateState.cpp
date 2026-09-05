#include "RotateState.h"

#include "../NavigationComponent.h"

namespace {
FString BuildTurnPrompt(float SignedAngleDegrees) {
  const float AbsDeg = FMath::Abs(SignedAngleDegrees);
  const int32 IntDeg = FMath::RoundToInt(AbsDeg);
  const FString Dir = (SignedAngleDegrees >= 0.0f) ? UTF8_TO_TCHAR(u8"右")
                                                   : UTF8_TO_TCHAR(u8"左");
  return FString::Printf(TEXT("%s%s%d%s"), UTF8_TO_TCHAR(u8"请向"), *Dir,
                         IntDeg, UTF8_TO_TCHAR(u8"度转身"));
}
}  // namespace

void FRotateState::OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {
  Nav.StopBeep();
  Nav.StopDrip();
  
  bBeepStarted = false;
  // 起始快照
  Ctx.StartLocation = Ctx.PlayerLoc;

  // 内部计时初始化
  LastHeadingSampleTime = Ctx.CurrentTime;
  LastHeadingDegrees = Ctx.AngleError;
  StartAngleError = Ctx.AngleError;   // 不变，用于计算转头量
  LastRepromptTime = Ctx.CurrentTime;
  LastAnnouncedAngle = 999.0f;

  // TTS 转向提示（已对准则跳过）
  if (FMath::Abs(Ctx.AngleError) >= Nav.AlignToleranceDegrees) {
    Nav.EnqueuePrompt(BuildTurnPrompt(Ctx.AngleError));
  }
}

void FRotateState::Tick(UNavigationComponent& Nav, FNavContext& Ctx) {
  const float AbsErr = FMath::Abs(Ctx.AngleError);

  // 静止检测
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
    // 已对准时不重复提示
    if (AbsErr >= Nav.AlignToleranceDegrees) {
      Nav.EnqueuePrompt(BuildTurnPrompt(Ctx.AngleError));
    }
  }

  // 蜂鸣：用户开始转之后才响，一旦启动就不再停止。
  {
    const float TurnedFromStart = FMath::Abs(FMath::FindDeltaAngleDegrees(
        StartAngleError, Ctx.AngleError));
    if (!bBeepStarted) {
      bBeepStarted = (TurnedFromStart >= Nav.AlignBeepStartDeltaDegrees);
    }
    if (bBeepStarted && Nav.SoundComp) {
      const float Progress = FMath::Clamp(1.0f - (AbsErr / 90.0f), 0.0f, 1.0f);
      // 转向校准：高频警报（600-1000Hz），比前进蜂鸣更尖锐，便于区分。
      const int32 FreqHz = FMath::RoundToInt(
          Nav.SoundComp->RotateBeepBaseFreqHz +
          Nav.SoundComp->RotateBeepFreqRangeHz * Progress * Progress);
      // 立体声 pan：每帧按当前 AngleError 的符号选择左/右声道。
      // 不使用角度大小计算 Pan，避免越接近目标越向中间收。
      const float PanDirection = FMath::Sign(Ctx.AngleError);
      const float Pan = PanDirection *
                        FMath::Clamp(Nav.SoundComp->RotatePanStrength, 0.0f, 1.0f);
      // 越接近对准，音量越低（避免快到位时还吵）
      const float Volume = FMath::Clamp(0.4f + 0.6f * (1.0f - Progress), 0.4f, 1.0f);
      // 蜂鸣间隔随 Progress 动态缩短（越对准更新越频繁）
      Nav.SoundComp->BeepUpdateIntervalSeconds =
          FMath::Lerp(1.0f, 0.2f, Progress * Progress);
      Nav.SendBeepCommand(true, FreqHz, Pan, Volume,
                          ENavSoundCategory::Beep_TurnCalibrate);
    }
  }

  // 接近提示：仅当角度收窄（接近对准）跨过阈值时播报
  {
    const float Thresholds[] = {30.0f, 10.0f};
    for (float T : Thresholds) {
      if (AbsErr <= T && PrevAbsError > T) {
        Nav.EnqueuePrompt(
            FString::Printf(TEXT("%.0f%s"), T,
                            UTF8_TO_TCHAR(u8"度")));
        break;
      }
    }
  }

  PrevAbsError = AbsErr;

    // 返回值被忽略，仅为满足基类接口
}

void FRotateState::OnExit(UNavigationComponent& Nav, FNavContext& Ctx) {
  // 离开 Rotate 时总是停连续引导音；只有真正对准并进入执行阶段时，
  // 才发送“校准成功”的音效和语音。偏离/重规划/停止导航也会触发 OnExit。
  Nav.StopBeep();
  Nav.StopDrip();

  const bool bHasValidWaypoint =
      Nav.CurrentWaypointIndex >= 0 && Nav.PlannedWaypoints.Num() >= 2;
  const bool bAligned =
      FMath::Abs(Ctx.AngleError) < Nav.AlignToleranceDegrees;
  if (!bHasValidWaypoint || !bAligned) {
    return;
  }

  Nav.ClearNonCriticalPrompts();
  Nav.SendSoundEffect(ENavSoundCategory::SFX_WaypointReached);
  Nav.EnqueuePrompt(UTF8_TO_TCHAR(u8"校准成功"));
}
