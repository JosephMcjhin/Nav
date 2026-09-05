#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "NavigationSoundPlayer.generated.h"

class USoundBase;

/** 导航音效类型：区分"连续蜂鸣引导"与"离散音效事件"。 */
UENUM(BlueprintType)
enum class ENavSoundCategory : uint8 {
  Beep_TurnCalibrate  UMETA(DisplayName = "Beep: Turn Calibrate"),
  SFX_WaypointReached UMETA(DisplayName = "SFX: Waypoint Reached"),
  SFX_Deviation       UMETA(DisplayName = "SFX: Deviation Error"),
  SFX_Arrival         UMETA(DisplayName = "SFX: Arrival"),
};

/**
 * 导航音效组件。
 * 作为 NavigationComponent 的子组件，集中管理所有音效、蜂鸣和 TTS 提示队列。
 */
UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UNavigationSoundComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UNavigationSoundComponent();
  virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                             FActorComponentTickFunction* ThisTickFunction) override;

  void Initialize(class UNavigationComponent* InNavComp);

  // ── 蜂鸣/音效 ─────────────────────────────────────────────────────
  void SendBeepCommand(bool bActive, int32 FreqHz, float Pan = 0.0f,
                       float Volume = 1.0f,
                       ENavSoundCategory BeepType = ENavSoundCategory::Beep_TurnCalibrate);
  void SendSoundEffect(ENavSoundCategory Category);
  void StopBeep();

  // ── 水滴引导（类似 beep 的消息模式：一次发送间隔，客户端自行循环） ──
  void SendDripCommand(bool bActive, float IntervalMs);
  void StopDrip();

  // ── TTS 提示队列 ──────────────────────────────────────────────────
  void EnqueuePrompt(const FString& Message, bool bHighPriority = false);
  void ClearNonCriticalPrompts();
  bool IsPromptPlaying(float CurrentTime) const {
    return CurrentTime < NextPromptDispatchTime;
  }

  // ── 转向校准蜂鸣参数 ──────────────────────────────────────────────
  UPROPERTY(EditAnywhere, Category = "Rotate")
  float RotateBeepBaseFreqHz = 800.0f;
  UPROPERTY(EditAnywhere, Category = "Rotate")
  float RotateBeepFreqRangeHz = 400.0f;
  UPROPERTY(EditAnywhere, Category = "Rotate")
  float RotatePanStrength = 1.0f;

  // ── 前进引导蜂鸣参数 ──────────────────────────────────────────────
  // ── 节流 ──────────────────────────────────────────────────────────
  UPROPERTY(EditAnywhere, Category = "Timing")
  float BeepUpdateIntervalSeconds = 0.5f;

  // ── TTS ───────────────────────────────────────────────────────────
  UPROPERTY(EditAnywhere, Category = "TTS")
  float PromptGapSeconds = 0.2f;

  // ── PC 端音效资产 ─────────────────────────────────────────────────
  UPROPERTY(EditAnywhere, Category = "Assets")
  USoundBase* SoundAssetDrip = nullptr;
  UPROPERTY(EditAnywhere, Category = "Assets")
  USoundBase* SoundAssetWaypoint = nullptr;
  UPROPERTY(EditAnywhere, Category = "Assets")
  USoundBase* SoundAssetDeviation = nullptr;
  UPROPERTY(EditAnywhere, Category = "Assets")
  USoundBase* SoundAssetArrival = nullptr;

private:
  float EstimatePromptDurationSeconds(const FString& Message) const;
  void ProcessPromptScheduler(float CurrentTime);
  void PlayLocalBeep(bool bActive, int32 FreqHz, float Pan, float Volume,
                     float IntervalMs, ENavSoundCategory BeepType);

  class UNavigationComponent* NavComp = nullptr;
  float LastBeepSendTime = 0.0f;
  float LastDripSendTime = 0.0f;
  float LastLocalDripTime = 0.0f;

  // TTS 待播队列：调度时只取最新一条，其余全部丢弃。
  TArray<FString> PendingPrompts;
  float NextPromptDispatchTime = 0.0f;
};

