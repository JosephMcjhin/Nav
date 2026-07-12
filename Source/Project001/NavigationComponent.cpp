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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ServerConnectionComponent.h"

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

void SendBeepMessage(AActor *OwnerActor, bool bActive, int32 FreqHz,
                     float Pan, float Volume, float IntervalMs) {
  if (!OwnerActor) return;
  if (UServerConnectionComponent *ServerComp =
          OwnerActor->FindComponentByClass<UServerConnectionComponent>()) {
    const FString JsonStr = FString::Printf(
        TEXT("{\"type\":\"nav_beep\",\"active\":%s,\"freq_hz\":%d,"
         "\"pan\":%.3f,\"volume\":%.3f,\"interval_ms\":%.0f}"),
        bActive ? TEXT("true") : TEXT("false"), FreqHz, Pan, Volume, IntervalMs);
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

void UNavigationComponent::EnqueuePrompt(const FString &Message, bool bInsertFirst) {
  if (Message.IsEmpty()) return;
  if (bInsertFirst) {
    PendingPrompts.Insert(Message, 0);
  } else {
    PendingPrompts.Add(Message);
  }
}

void UNavigationComponent::ClearNonCriticalPrompts() {
  PendingPrompts.Empty();
  CurrentRealtimePrompt.Empty();
  bHasRealtimePromptPending = false;
}

void UNavigationComponent::ProcessPromptScheduler(float CurrentTime) {
  // TTS 播放中 → 等待播完再发下一条
  if (CurrentTime < NextPromptDispatchTime) return;

  FString MessageToSend;
  if (PendingPrompts.Num() > 0) {
    MessageToSend = PendingPrompts[0];
    PendingPrompts.RemoveAt(0);
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
    EnqueuePrompt(UTF8_TO_TCHAR(u8"未知的目的地"));
    return false;
  }
  ActiveTarget = DestinationTag;
  ActiveTargetLocation = DestinationMap[DestinationTag];
  bIsNavigating = true;
  PlannedWaypoints.Empty();
  CurrentWaypointIndex = -1;
  LastReplanCheckTime = 0.0f;
  PendingPrompts.Empty();
  ClearNonCriticalPrompts();
  EnqueuePrompt(
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
  EnqueuePrompt(UTF8_TO_TCHAR(u8"导航结束"));
  FinishNavigation(false);
}

// ── 远程导航参数配置 ───────────────────────────────────────────────────────────
// 用一个宏表把"JSON 键名 → UPROPERTY 字段"集中管理，避免重复样板代码。
// 每条记录包含：键名、字段类型、setter lambda。新增可配置字段只需在此处加一行。
namespace {
struct FRemoteConfigField {
  const TCHAR *Key;       // JSON 键名（必须与 nav_config.json 一致）
  enum EType { Float, Bool } Type;
};

// 注意：顺序与下面 ApplyRemoteConfig 的 switch 分支一一对应。
static const FRemoteConfigField GRemoteConfigFields[] = {
    {TEXT("ArrivalDistanceMeters"),          FRemoteConfigField::Float},
    {TEXT("AlignBeepStartDeltaDegrees"),     FRemoteConfigField::Float},
    {TEXT("AlignToleranceDegrees"),          FRemoteConfigField::Float},
    {TEXT("ExecuteBeepStartMeters"),         FRemoteConfigField::Float},
    {TEXT("ExecuteDriftDegrees"),            FRemoteConfigField::Float},
    {TEXT("AlignIdleRepromptSeconds"),       FRemoteConfigField::Float},
    {TEXT("PromptGapSeconds"),               FRemoteConfigField::Float},
    {TEXT("RouteDeviationThresholdMeters"),  FRemoteConfigField::Float},
    {TEXT("RouteReplanCheckIntervalSeconds"), FRemoteConfigField::Float},
    {TEXT("WaypointReachRadiusMeters"),      FRemoteConfigField::Float},
};
}  // namespace

bool UNavigationComponent::ApplyRemoteConfig(const FString &JsonString) {
  if (JsonString.IsEmpty()) {
    OnRemoteConfigApplied.Broadcast(false, 0, TEXT("Empty JSON payload"));
    return false;
  }

  TSharedPtr<FJsonObject> RootObject;
  {
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid()) {
      OnRemoteConfigApplied.Broadcast(false, 0, TEXT("JSON parse failed"));
      return false;
    }
  }

  // 兼容两种包裹形式：
  //   1) { "type":"nav_config", "config": { ... } }  ← 后端标准格式
  //   2) { ... }                                       ← 直接平铺
  const TSharedPtr<FJsonObject>* ConfigObjPtr = nullptr;
  if (RootObject->HasField(TEXT("config"))) {
    ConfigObjPtr = &RootObject->GetObjectField(TEXT("config"));
  }
  const TSharedPtr<FJsonObject>& ConfigObj =
      (ConfigObjPtr && ConfigObjPtr->IsValid()) ? *ConfigObjPtr : RootObject;

  int32 AppliedCount = 0;
  TArray<FString> AppliedKeys;

  for (const FRemoteConfigField &Field : GRemoteConfigFields) {
    if (!ConfigObj->HasField(Field.Key)) {
      continue;  // 该字段未提供，保留当前值
    }

    if (Field.Type == FRemoteConfigField::Float) {
      double Value = 0.0;
      if (!ConfigObj->TryGetNumberField(Field.Key, Value)) {
        UE_LOG(LogTemp, Warning,
               TEXT("[RemoteConfig] Field '%s' wrong type (expected number), skipped"),
               Field.Key);
        continue;
      }
      const float FValue = static_cast<float>(Value);

      // 注意：与 GRemoteConfigFields 数组顺序一一对应
      if (FCString::Strcmp(Field.Key, TEXT("ArrivalDistanceMeters")) == 0)
        ArrivalDistanceMeters = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("AlignBeepStartDeltaDegrees")) == 0)
        AlignBeepStartDeltaDegrees = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("AlignToleranceDegrees")) == 0)
        AlignToleranceDegrees = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("ExecuteBeepStartMeters")) == 0)
        ExecuteBeepStartMeters = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("ExecuteDriftDegrees")) == 0)
        ExecuteDriftDegrees = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("AlignIdleRepromptSeconds")) == 0)
        AlignIdleRepromptSeconds = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("PromptGapSeconds")) == 0)
        PromptGapSeconds = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("RouteDeviationThresholdMeters")) == 0)
        RouteDeviationThresholdMeters = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("RouteReplanCheckIntervalSeconds")) == 0)
        RouteReplanCheckIntervalSeconds = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("WaypointReachRadiusMeters")) == 0)
        WaypointReachRadiusMeters = FValue;

      AppliedKeys.Add(FString::Printf(TEXT("%s=%.3f"), Field.Key, FValue));
      ++AppliedCount;
    }
  }

  UE_LOG(LogTemp, Log,
         TEXT("[RemoteConfig] Applied %d fields: %s"),
         AppliedCount, *FString::Join(AppliedKeys, TEXT(", ")));

  if (GEngine) {
    GEngine->AddOnScreenDebugMessage(
        8888, 4.0f,
        AppliedCount > 0 ? FColor::Green : FColor::Yellow,
        FString::Printf(TEXT("[RemoteConfig] Applied %d fields"), AppliedCount));
  }

  const FString Message = AppliedCount > 0
      ? FString::Printf(TEXT("Applied %d config fields"), AppliedCount)
      : TEXT("No applicable fields found in config");
  OnRemoteConfigApplied.Broadcast(AppliedCount > 0, AppliedCount, Message);
  return AppliedCount > 0;
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

void UNavigationComponent::PlayLocalBeep(bool bActive, int32 FreqHz,
                                         float Pan, float Volume, float IntervalMs) {
  Project001Console::PlayLocalBeep(bActive, FreqHz, Pan, Volume, IntervalMs);
}

void UNavigationComponent::SendBeepCommand(bool bActive, int32 FreqHz,
                                         float Pan, float Volume) {
  const float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
  if (bActive &&
      (CurrentTime - LastBeepSendTime) < BeepUpdateIntervalSeconds) return;
  SendBeepMessage(GetOwner(), bActive, FreqHz, Pan, Volume,
                   BeepUpdateIntervalSeconds * 1000.0f);
  if (Project001Console::IsLocalNavTTSEnabled()) PlayLocalBeep(bActive, FreqHz, Pan, Volume, BeepUpdateIntervalSeconds * 1000.0f);
  LastBeepSendTime = CurrentTime;
}

void UNavigationComponent::StopBeep() {
  SendBeepMessage(GetOwner(), false, 0, 0.0f, 1.0f, 0.0f);
  if (Project001Console::IsLocalNavTTSEnabled())
    Project001Console::PlayLocalBeep(false, 0, 0.0f, 1.0f, 0.0f);
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

    // 构建时间戳（Key=9998 持续显示，便于打包后版本确认）
    GEngine->AddOnScreenDebugMessage(
        9998, 0.f, FColor::Cyan,
        FString::Printf(TEXT("Build: %s"),
                        *Project001Console::GetBuildTimestamp()));

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
    // 清掉之前的提示，只播到达信息
    ClearNonCriticalPrompts();
    const FVector ToTarget2D =
        (ActiveTargetLocation - NavCtx.PlayerLoc).GetSafeNormal2D();
    const FVector Forward2D = NavCtx.PlayerForward.GetSafeNormal2D();
    const FVector Right2D = NavCtx.PlayerRight.GetSafeNormal2D();
    const FString DirectionPhrase =
        UNavigationMathLibrary::GetRelativeDirectionPhrase(
            Forward2D, Right2D, ToTarget2D);

    EnqueuePrompt(
        UNavigationMathLibrary::GetDestinationDisplayName(ActiveTarget) +
        FString::Printf(TEXT("%s%s"),
                        UTF8_TO_TCHAR(u8"到了，在你"),
                        *DirectionPhrase),
        true);
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
      EnqueuePrompt(UTF8_TO_TCHAR(u8"已偏离路线，重新规划路线"));
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
      ClearNonCriticalPrompts();
      EnqueuePrompt(UTF8_TO_TCHAR(u8"到达路点。"), true);
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