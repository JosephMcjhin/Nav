#include "NavigationComponent.h"

#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationMathLibrary.h"
#include "NavigationSystem.h"
#include "NavigationStateMachine.h"
#include "Project001.h"
#include "ServerConnectionComponent.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace {
void ShowNavDebugMessage(int32 Key, const FString &Message,
                         const FColor &Color = FColor::Cyan,
                         float Duration = 0.5f) {
  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(Key, Duration, Color, Message);
  }
}

void SendNavPromptMessage(AActor *OwnerActor, const FString &Message) {
  if (!OwnerActor || Message.IsEmpty()) return;
  Project001Console::SpeakLocalNavText(Message);
  if (UServerConnectionComponent *ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr =
        FString::Printf(TEXT("{\"type\":\"nav_prompt\",\"text\":\"%s\"}"), *Message);
    ServerComp->SendString(JsonStr);
  }
}

void SendBeepMessage(AActor *OwnerActor, bool bActive, int32 FreqHz) {
  if (!OwnerActor) return;
  if (UServerConnectionComponent *ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr = FString::Printf(
        TEXT("{\"type\":\"nav_beep\",\"active\":%s,\"freq_hz\":%d}"),
        bActive ? TEXT("true") : TEXT("false"), FreqHz);
    ServerComp->SendString(JsonStr);
  }
}
}  // namespace

UNavigationComponent::UNavigationComponent() {
  PrimaryComponentTick.bCanEverTick = true;
  DestinationTags.Add(FName(TEXT("WorkStation")));
  DestinationTags.Add(FName(TEXT("ConferenceTable")));
  DestinationTags.Add(FName(TEXT("Sofa")));
  DestinationTags.Add(FName(TEXT("AirConditioner")));
  DestinationTags.Add(FName(TEXT("Door")));
}

void UNavigationComponent::BeginPlay() {
  Super::BeginPlay();
  CachedNavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
  RefreshDestinationMap();
}

void UNavigationComponent::RefreshDestinationMap() {
  DestinationMap.Empty();
  for (const FName &Tag : DestinationTags) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);
    for (AActor *Actor : FoundActors) {
      if (!Actor) continue;
      FNavLocation ProjectedLoc;
      const FVector ActorLoc = Actor->GetActorLocation();
      if (CachedNavSys && CachedNavSys->ProjectPointToNavigation(
                              ActorLoc, ProjectedLoc, FVector(200.f))) {
        DestinationMap.Add(Tag, ProjectedLoc.Location);
      } else {
        DestinationMap.Add(Tag, ActorLoc);
      }
      break;
    }
  }
}

float UNavigationComponent::EstimatePromptDurationSeconds(
    const FString &Message) const {
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
  return DurationSeconds / 2.0f;
}

void UNavigationComponent::EnqueueHighPriorityPrompt(const FString &Message) {
  if (!Message.IsEmpty()) HighPriorityPrompts.Add(Message);
}

void UNavigationComponent::EnqueueMediumPriorityPrompt(const FString &Message) {
  if (!Message.IsEmpty()) MediumPriorityPrompts.Add(Message);
}

void UNavigationComponent::ClearNonCriticalPrompts() {
  MediumPriorityPrompts.Empty();
  CurrentRealtimePrompt.Empty();
  bHasRealtimePromptPending = false;
}

void UNavigationComponent::ProcessPromptScheduler(float CurrentTime) {
  FString MessageToSend;
  if (HighPriorityPrompts.Num() > 0) {
    MessageToSend = HighPriorityPrompts[0];
    HighPriorityPrompts.RemoveAt(0);
  } else if (MediumPriorityPrompts.Num() > 0) {
    MessageToSend = MediumPriorityPrompts[0];
    MediumPriorityPrompts.RemoveAt(0);
  } else if (bHasRealtimePromptPending) {
    MessageToSend = CurrentRealtimePrompt;
    bHasRealtimePromptPending = false;
  }
  if (MessageToSend.IsEmpty()) return;
  SendNavPromptMessage(GetOwner(), MessageToSend);
  NextPromptDispatchTime =
      CurrentTime + EstimatePromptDurationSeconds(MessageToSend) +
      PromptGapSeconds;
}

bool UNavigationComponent::NavigateTo(FName DestinationTag) {
  if (!GetOwner()) return false;
  if (!DestinationMap.Contains(DestinationTag)) {
    EnqueueHighPriorityPrompt(UTF8_TO_TCHAR(u8"未知的目的地"));
    return false;
  }
  ActiveTarget = DestinationTag;
  ActiveTargetLocation = DestinationMap[DestinationTag];
  bIsNavigating = true;
  PlannedWaypoints.Empty();
  CurrentWaypointIndex = -1;
  LastReplanCheckTime = 0.0f;
  HighPriorityPrompts.Empty();
  ClearNonCriticalPrompts();
  EnqueueHighPriorityPrompt(
      UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget) +
      UTF8_TO_TCHAR(u8"导航开始。"));
  // 不在此处 SwitchTo(Plan)：NavCtx 还没填充。
  // 下一帧 TickComponent 会填充 NavCtx，状态机检测到 CurrentWaypointIndex
  // 从 -2 变 -1 → 自动切 Plan，此时 OnEnter 能用到正确的 PlayerLoc。
  OnNavigationStarted.Broadcast(DestinationTag);
  return true;
}

void UNavigationComponent::StopNavigation() {
  ClearNonCriticalPrompts();
  EnqueueHighPriorityPrompt(UTF8_TO_TCHAR(u8"导航结束"));
  FinishNavigation(false);
}

TArray<FName> UNavigationComponent::GetAvailableDestinations() const {
  TArray<FName> Keys;
  DestinationMap.GetKeys(Keys);
  return Keys;
}

void UNavigationComponent::FinishNavigation(bool bSuccess) {
  const FName FinishedTarget = ActiveTarget;
  bIsNavigating = false;
  ActiveTarget = NAME_None;
  PlannedWaypoints.Empty();
  CurrentWaypointIndex = -1;
  StopBeep();
  ClearNonCriticalPrompts();
  StateMachine.Reset();
  OnNavigationArrived.Broadcast(FinishedTarget, bSuccess);
}

float UNavigationComponent::ComputePathDistanceMeters(
    const TArray<FVector> &PathPoints) const {
  float Total = 0.0f;
  for (int32 i = 0; i < PathPoints.Num() - 1; ++i) {
    Total += FVector::Dist(PathPoints[i], PathPoints[i + 1]);
  }
  return Total / 100.0f / DistanceScale;
}

void UNavigationComponent::HandleDebugKeyboardInput(AActor *Owner, float DeltaTime) {
  APawn *Pawn = Cast<APawn>(Owner);
  if (!Pawn) return;
  APlayerController *PC = GetWorld()->GetFirstPlayerController();
  if (!PC) return;
  const float Turn = DebugTurnRateDegreesPerSec * DeltaTime;
  if (PC->IsInputKeyDown(EKeys::Q)) Pawn->AddActorLocalRotation(FRotator(0, -Turn, 0));
  if (PC->IsInputKeyDown(EKeys::E)) Pawn->AddActorLocalRotation(FRotator(0, Turn, 0));
  if (PC->IsInputKeyDown(EKeys::R)) Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.0f);
}

void UNavigationComponent::PlayLocalBeep(int32 FreqHz) {
#if PLATFORM_WINDOWS
  if (FreqHz > 0) ::Beep((DWORD)FreqHz, 30);
#endif
}

void UNavigationComponent::SendBeepCommand(bool bActive, int32 FreqHz) {
  const float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
  if (bActive && bBeepActive &&
      (CurrentTime - LastBeepSendTime) < BeepUpdateIntervalSeconds) return;
  SendBeepMessage(GetOwner(), bActive, FreqHz);
  if (Project001Console::IsLocalNavTTSEnabled()) PlayLocalBeep(FreqHz);
  bBeepActive = bActive;
  LastBeepSendTime = CurrentTime;
}

void UNavigationComponent::StopBeep() {
  if (bBeepActive) {
    SendBeepMessage(GetOwner(), false, 0);
    bBeepActive = false;
  }
}

void UNavigationComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  AActor *Owner = GetOwner();
  if (!Owner) return;

  if (bEnableDebugKeyboardControl) HandleDebugKeyboardInput(Owner, DeltaTime);

  const float CurrentTime = GetWorld()->GetTimeSeconds();

  // 每帧打印当前状态（屏幕左上角，Key=9999）
  if (bShowDebugMessages && GEngine) {
    const FName StateName = StateMachine.GetCurrentStatePtr()
                                ? StateMachine.GetCurrentStatePtr()->GetName()
                                : FName(TEXT("NONE"));
    GEngine->AddOnScreenDebugMessage(
        9999, 0.f, FColor::Green,
        FString::Printf(TEXT("NavState: %s | wp:%d/%d"),
                        *StateName.ToString(),
                        CurrentWaypointIndex,
                        FMath::Max(0, PlannedWaypoints.Num() - 1)));
  }

  if (!bIsNavigating || !CachedNavSys || ActiveTarget == NAME_None) {
    ProcessPromptScheduler(CurrentTime);
    return;
  }

  // 填充上下文
  NavCtx.PlayerLoc = Owner->GetActorLocation();
  NavCtx.PlayerForward = Owner->GetActorForwardVector();
  NavCtx.PlayerRight = Owner->GetActorRightVector();
  NavCtx.CurrentTime = CurrentTime;

  const float DirectDist =
      FVector::Dist2D(NavCtx.PlayerLoc, ActiveTargetLocation) / 100.0f / DistanceScale;

  // 到达
  if (DirectDist <= ArrivalDistanceMeters) {
    ClearNonCriticalPrompts();
    EnqueueHighPriorityPrompt(
        UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget) +
        UTF8_TO_TCHAR(u8"到了"));
    FinishNavigation(true);
    ProcessPromptScheduler(CurrentTime);
    LastPlayerLocation = NavCtx.PlayerLoc;
    return;
  }

  // 投影目标点
  FNavLocation ProjectedTarget;
  if (!CachedNavSys->ProjectPointToNavigation(ActiveTargetLocation, ProjectedTarget, FVector(200.f))) {
    ProcessPromptScheduler(CurrentTime);
    LastPlayerLocation = NavCtx.PlayerLoc;
    return;
  }

  // Tick 状态机（EvaluateState + OnEnter + Tick）
  StateMachine.Tick(*this, NavCtx);

  // 偏差检测（仅在正常导航中）
  if (CurrentWaypointIndex >= 0 && PlannedWaypoints.Num() >= 2 &&
      (CurrentTime - LastReplanCheckTime) > RouteReplanCheckIntervalSeconds) {
    LastReplanCheckTime = CurrentTime;

    // 检测 A：全局实时路径比计划路径距离短很多 → 偏移
    UNavigationPath *LivePath = CachedNavSys->FindPathToLocationSynchronously(
        GetWorld(), NavCtx.PlayerLoc, ProjectedTarget.Location);
    bool bDeviated = false;
    if (LivePath && LivePath->PathPoints.Num() >= 2) {
      float LiveTotal = 0.0f;
      for (int32 i = 0; i < LivePath->PathPoints.Num() - 1; ++i)
        LiveTotal += FVector::Dist(LivePath->PathPoints[i], LivePath->PathPoints[i + 1]);
      LiveTotal /= 100.0f * DistanceScale;

      float PlannedRemain =
          FVector::Dist2D(NavCtx.PlayerLoc, PlannedWaypoints[CurrentWaypointIndex]);
      for (int32 i = CurrentWaypointIndex; i < PlannedWaypoints.Num() - 1; ++i)
        PlannedRemain += FVector::Dist(PlannedWaypoints[i], PlannedWaypoints[i + 1]);
      PlannedRemain /= 100.0f * DistanceScale;

      bDeviated = PlannedRemain - LiveTotal > RouteDeviationThresholdMeters;
    }

    // 检测 B：玩家到下一个路点的路径多了额外路点，且偏离 > 0.5m
    if (!bDeviated) {
      UNavigationPath *SegPath = CachedNavSys->FindPathToLocationSynchronously(
          GetWorld(), NavCtx.PlayerLoc,
          PlannedWaypoints[CurrentWaypointIndex]);
      if (SegPath && SegPath->PathPoints.Num() > 2) {
        const float DistToFirstExtra =
            FVector::Dist(NavCtx.PlayerLoc, SegPath->PathPoints[1]) /
            100.0f / DistanceScale;
        bDeviated = DistToFirstExtra > 1.0f;
      }
    }

    if (bDeviated) {
      EnqueueHighPriorityPrompt(UTF8_TO_TCHAR(u8"已偏离路线，重新规划路线"));
      CurrentWaypointIndex = -1;
      LastReplanCheckTime = CurrentTime;
    }
  }

  // 未就绪则跳过
  if (CurrentWaypointIndex < 0 || PlannedWaypoints.Num() < 2) {
    ProcessPromptScheduler(CurrentTime);
    LastPlayerLocation = NavCtx.PlayerLoc;
    return;
  }

  // 路点推进：到达当前路点 → 切到下一个路点
  {
    const FVector CurrWP = PlannedWaypoints[FMath::Min(CurrentWaypointIndex,
                                                       PlannedWaypoints.Num() - 1)];
    const float DistToCurrWP =
        FVector::Dist2D(NavCtx.PlayerLoc, CurrWP) / 100.0f / DistanceScale;
    if (DistToCurrWP <= WaypointReachRadiusMeters &&
        CurrentWaypointIndex < PlannedWaypoints.Num() - 1) {
      EnqueueHighPriorityPrompt(UTF8_TO_TCHAR(u8"到达路点。"));
      CurrentWaypointIndex++;
      StopBeep();
    }
  }

  // Debug: 画玩家到下一个路点的 navmesh 真实导航路线。
  {
    // const FVector NW =
    //     PlannedWaypoints[FMath::Min(CurrentWaypointIndex,
    //                                 PlannedWaypoints.Num() - 1)];
    // DrawDebugLine(GetWorld(), NavCtx.PlayerLoc, NW, FColor::Yellow, false,
    //               0.1f, 0, 1.0f);
    UNavigationPath *SegPath = CachedNavSys->FindPathToLocationSynchronously(
        GetWorld(), NavCtx.PlayerLoc,
        PlannedWaypoints[FMath::Min(CurrentWaypointIndex,
                                    PlannedWaypoints.Num() - 1)]);
    if (SegPath && SegPath->PathPoints.Num() >= 2) {
      for (int32 i = 0; i < SegPath->PathPoints.Num() - 1; ++i) {
        DrawDebugLine(GetWorld(), SegPath->PathPoints[i],
                      SegPath->PathPoints[i + 1], FColor::Yellow, false,
                      0.1f, 0, 1.0f);
      }
    }
  }

  ProcessPromptScheduler(CurrentTime);
  LastPlayerLocation = NavCtx.PlayerLoc;
}