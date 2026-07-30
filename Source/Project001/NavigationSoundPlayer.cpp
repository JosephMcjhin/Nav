#include "NavigationSoundPlayer.h"

#include "NavigationComponent.h"
#include "Project001.h"
#include "ServerConnectionComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"

// ============================================================================
// 辅助函数（匿名命名空间）
// ============================================================================
namespace {
void SendBeepMessage(AActor* OwnerActor, bool bActive, int32 FreqHz,
                     float Pan, float Volume, float IntervalMs,
                     const FString& BeepTypeStr) {
  if (!OwnerActor) return;
  if (UServerConnectionComponent* ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr = FString::Printf(
        TEXT("{\"type\":\"nav_beep\",\"active\":%s,\"freq_hz\":%d,"
             "\"pan\":%.3f,\"volume\":%.3f,\"interval_ms\":%.0f,"
             "\"beep_type\":\"%s\"}"),
        bActive ? TEXT("true") : TEXT("false"), FreqHz, Pan, Volume,
        IntervalMs, *BeepTypeStr);
    ServerComp->SendString(JsonStr);
  }
}

void SendSoundMessage(AActor* OwnerActor, const FString& SoundId) {
  if (!OwnerActor || SoundId.IsEmpty()) return;
  if (UServerConnectionComponent* ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr =
        FString::Printf(TEXT("{\"type\":\"nav_sound\",\"sound\":\"%s\"}"),
                        *SoundId);
    ServerComp->SendString(JsonStr);
  }
}

FString BeepTypeToString(ENavSoundCategory Cat) {
  switch (Cat) {
  case ENavSoundCategory::Beep_TurnCalibrate:
    return TEXT("turn_calibrate");
  default:
    return TEXT("turn_calibrate");
  }
}

FString SoundCategoryToId(ENavSoundCategory Cat) {
  switch (Cat) {
  case ENavSoundCategory::SFX_WaypointReached:
    return TEXT("waypoint");
  case ENavSoundCategory::SFX_Deviation:
    return TEXT("deviation");
  case ENavSoundCategory::SFX_Arrival:
    return TEXT("arrival");
  default:
    return TEXT("");
  }
}
}  // namespace

// ============================================================================
// UNavigationSoundComponent
// ============================================================================
UNavigationSoundComponent::UNavigationSoundComponent() {
  PrimaryComponentTick.bCanEverTick = true;
}

void UNavigationSoundComponent::Initialize(UNavigationComponent* InNavComp) {
  NavComp = InNavComp;
  LastBeepSendTime = 0.0f;
}

void UNavigationSoundComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
  ProcessPromptScheduler(GetWorld()->GetTimeSeconds());
}

void UNavigationSoundComponent::SendBeepCommand(bool bActive, int32 FreqHz,
                                                 float Pan, float Volume,
                                                 ENavSoundCategory BeepType) {
  AActor* OwnerActor = GetOwner();
  const float CurrentTime =
      OwnerActor ? OwnerActor->GetWorld()->GetTimeSeconds() : 0.0f;
  if (bActive &&
      (CurrentTime - LastBeepSendTime) < BeepUpdateIntervalSeconds) {
    return;
  }

  const FString TypeStr = BeepTypeToString(BeepType);
  SendBeepMessage(OwnerActor, bActive, FreqHz, Pan, Volume,
                  BeepUpdateIntervalSeconds * 1000.0f, TypeStr);
  if (Project001Console::IsLocalNavTTSEnabled()) {
    PlayLocalBeep(bActive, FreqHz, Pan, Volume,
                  BeepUpdateIntervalSeconds * 1000.0f, BeepType);
  }
  LastBeepSendTime = CurrentTime;
}

void UNavigationSoundComponent::SendSoundEffect(ENavSoundCategory Category) {
  AActor* OwnerActor = GetOwner();
  const FString SoundId = SoundCategoryToId(Category);
  if (SoundId.IsEmpty()) return;

  SendSoundMessage(OwnerActor, SoundId);

  if (!Project001Console::IsLocalNavTTSEnabled()) return;

  USoundBase* Asset = nullptr;
  switch (Category) {
  case ENavSoundCategory::SFX_WaypointReached: Asset = SoundAssetWaypoint;  break;
  case ENavSoundCategory::SFX_Deviation:       Asset = SoundAssetDeviation; break;
  case ENavSoundCategory::SFX_Arrival:         Asset = SoundAssetArrival;   break;
  default: break;
  }

  if (Asset && OwnerActor) {
    UGameplayStatics::PlaySound2D(OwnerActor->GetWorld(), Asset);
  }
}

void UNavigationSoundComponent::StopBeep() {
  AActor* OwnerActor = GetOwner();
  SendBeepMessage(OwnerActor, false, 0, 0.0f, 1.0f, 0.0f, TEXT(""));
  if (Project001Console::IsLocalNavTTSEnabled()) {
    Project001Console::PlayLocalBeep(false, 0, 0.0f, 1.0f, 0.0f);
  }
}

void UNavigationSoundComponent::SendDripCommand(bool bActive,
                                                  float IntervalMs) {
  AActor* OwnerActor = GetOwner();
  const float CurrentTime =
      OwnerActor ? OwnerActor->GetWorld()->GetTimeSeconds() : 0.0f;
  if (bActive &&
      (CurrentTime - LastDripSendTime) < BeepUpdateIntervalSeconds) {
    return;
  }

  if (UServerConnectionComponent* ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr = FString::Printf(
        TEXT("{\"type\":\"nav_drip\",\"active\":%s,\"interval_ms\":%.0f}"),
        bActive ? TEXT("true") : TEXT("false"), IntervalMs);
    ServerComp->SendString(JsonStr);
  }

  // 本地 PC 端播放：按间隔播放水滴 WAV
  if (bActive && Project001Console::IsLocalNavTTSEnabled() && SoundAssetDrip) {
    if (CurrentTime - LastLocalDripTime >= IntervalMs / 1000.0f) {
      UGameplayStatics::PlaySound2D(OwnerActor->GetWorld(), SoundAssetDrip);
      LastLocalDripTime = CurrentTime;
    }
  }

  LastDripSendTime = CurrentTime;
}

void UNavigationSoundComponent::StopDrip() {
  SendDripCommand(false, 0.0f);
}

void UNavigationSoundComponent::PlayLocalBeep(bool bActive, int32 FreqHz,
                                               float Pan, float Volume,
                                               float IntervalMs,
                                               ENavSoundCategory BeepType) {
  Project001Console::PlayLocalBeep(bActive, FreqHz, Pan, Volume, IntervalMs);
}

// ============================================================================
// TTS 提示队列
// ============================================================================
void UNavigationSoundComponent::EnqueuePrompt(const FString& Message,
                                               bool bHighPriority) {
  if (Message.IsEmpty()) return;
  if (bHighPriority) {
    HighPriorityPrompts.Add(Message);
  } else {
    LowPriorityPrompts.Add(Message);
  }
}

void UNavigationSoundComponent::ClearNonCriticalPrompts() {
  LowPriorityPrompts.Empty();
  HighPriorityPrompts.Empty();
  CurrentRealtimePrompt.Empty();
  bHasRealtimePromptPending = false;
}

float UNavigationSoundComponent::EstimatePromptDurationSeconds(
    const FString& Message) const {
  float DurationSeconds = 0.2f;
  for (const TCHAR Char : Message) {
    switch (Char) {
    case 0xFF0C:
    case TEXT(','):
      DurationSeconds += 0.18f;
      break;
    case 0x3002:
    case TEXT('.'):
    case 0xFF01:
    case TEXT('!'):
    case 0xFF1F:
    case TEXT('?'):
      DurationSeconds += 0.35f;
      break;
    case TEXT(' '):
      DurationSeconds += 0.05f;
      break;
    default:
      DurationSeconds += 0.35f;
      break;
    }
  }
  return DurationSeconds / FMath::Max(TTSSpeedMultiplier, 0.1f);
}

void UNavigationSoundComponent::ProcessPromptScheduler(float CurrentTime) {
  if (CurrentTime < NextPromptDispatchTime) return;

  FString MessageToSend;
  if (HighPriorityPrompts.Num() > 0) {
    MessageToSend = HighPriorityPrompts[0];
    HighPriorityPrompts.RemoveAt(0);
  } else if (LowPriorityPrompts.Num() > 0) {
    MessageToSend = LowPriorityPrompts[0];
    LowPriorityPrompts.RemoveAt(0);
  } else if (bHasRealtimePromptPending) {
    MessageToSend = CurrentRealtimePrompt;
    bHasRealtimePromptPending = false;
  }
  if (MessageToSend.IsEmpty()) return;

  AActor* OwnerActor = GetOwner();
  if (!OwnerActor) return;

  Project001Console::SpeakLocalNavText(MessageToSend, TTSSpeedMultiplier);
  if (UServerConnectionComponent* ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr = FString::Printf(
        TEXT("{\"type\":\"nav_prompt\",\"text\":\"%s\",\"speed\":%.1f}"),
        *MessageToSend, TTSSpeedMultiplier);
    ServerComp->SendString(JsonStr);
  }
  NextPromptDispatchTime =
      CurrentTime + EstimatePromptDurationSeconds(MessageToSend) +
      PromptGapSeconds;
}
