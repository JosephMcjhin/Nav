#include "NavigationComponent.h"

#include "Blueprint/UserWidget.h"
#include "DrawDebugHelpers.h"
#include "TurnControlsWidget.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationMathLibrary.h"
#include "NavigationSystem.h"
#include "NavFilters/NavigationQueryFilter.h"
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

  if (!SoundComp) {
    SoundComp = GetOwner()->FindComponentByClass<UNavigationSoundComponent>();
  }
  if (SoundComp) SoundComp->Initialize(this);

  // 创建转向控制按钮 Widget
  if (bShowTurnControls && TurnControlsWidgetClass) {
    APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
    if (PC) {
      TurnControlsWidgetInstance = CreateWidget<UUserWidget>(PC, TurnControlsWidgetClass);
      if (TurnControlsWidgetInstance) {
        TurnControlsWidgetInstance->AddToViewport();
        // 传入导航组件引用
        if (UTurnControlsWidget* TurnWidget = Cast<UTurnControlsWidget>(TurnControlsWidgetInstance)) {
          TurnWidget->SetNavigationComponent(this);
        }
      }
    }
  }
}

void UNavigationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason) {
  StopBeep();
  StopDrip();
  if (TurnControlsWidgetInstance) {
    TurnControlsWidgetInstance->RemoveFromParent();
    TurnControlsWidgetInstance = nullptr;
  }
  Super::EndPlay(EndPlayReason);
}

void UNavigationComponent::RefreshDestinationMap() {
  DestinationMap.Empty();
  DestinationBounds.Empty();
  for (const FName &Tag : DestinationTags) {
    TArray<AActor *> FoundActors;
    UGameplayStatics::GetAllActorsWithTag(GetWorld(), Tag, FoundActors);
    for (AActor *Actor : FoundActors) {
      if (!Actor) continue;
      const FVector ActorLoc = Actor->GetActorLocation();

      if (bUseActorCenterAsDestination) {
        // 直接使用 Actor 中心（如 NavModifierVolume 的中心）
        // 不在此时投影：玩家方向未知，预先投影会锁死一个固定边缘点
        DestinationMap.Add(Tag, ActorLoc);
        // NavModifierVolume 没有普通几何组件，GetComponentsBoundingBox() 返回零。
        // 用 GetActorBounds 从 BrushComponent 获取真正体积。
        {
          FVector Origin;
          FVector BoxExtent;
          Actor->GetActorBounds(false, Origin, BoxExtent);
          DestinationBounds.Add(Tag, FBox(Origin - BoxExtent, Origin + BoxExtent));
        }
      } else {
        // 投影到碰撞体边缘的 navmesh
        FNavLocation ProjectedLoc;
        if (CachedNavSys && CachedNavSys->ProjectPointToNavigation(
                                ActorLoc, ProjectedLoc,
                                FVector(DestinationProjectionRadiusCm))) {
          DestinationMap.Add(Tag, ProjectedLoc.Location);
        } else {
          DestinationMap.Add(Tag, ActorLoc);
        }
      }
      break;
    }
  }
}

void UNavigationComponent::ReplanFromDeviation(const FNavContext& Ctx) {
  if (!CachedNavSys || ActiveTarget == NAME_None) return;

  const FVector PathEnd = GetProjectedTargetForPlayer(Ctx.PlayerLoc);

  if (PathfindingFilterClass) {
    ANavigationData* NavData = CachedNavSys->GetDefaultNavDataInstance();
    if (NavData) {
      FSharedConstNavQueryFilter QueryFilter =
          UNavigationQueryFilter::GetQueryFilter(
              *NavData, GetWorld(), PathfindingFilterClass);
      FPathFindingQuery Query(GetWorld(), *NavData, Ctx.PlayerLoc,
                              PathEnd, QueryFilter);
      FPathFindingResult Result =
          CachedNavSys->FindPathSync(Query, EPathFindingMode::Regular);
      if (Result.IsSuccessful() && Result.Path.IsValid()) {
        PlannedWaypoints.Empty();
        for (const FNavPathPoint& Pt : Result.Path->GetPathPoints()) {
          PlannedWaypoints.Add(Pt.Location);
        }
        if (PlannedWaypoints.Num() > 0) {
          CurrentWaypointIndex = 1;
          LastReplanCheckTime = Ctx.CurrentTime;
          return;
        }
      }
    }
  }

  // 回退：不带自定义过滤器（如果 PathfindingFilterClass 有值则带上）
  UNavigationPath* Path = CachedNavSys->FindPathToLocationSynchronously(
      GetWorld(), Ctx.PlayerLoc, PathEnd, nullptr, PathfindingFilterClass);
  if (Path && Path->PathPoints.Num() > 0) {
    PlannedWaypoints = Path->PathPoints;
    CurrentWaypointIndex = 1;
    LastReplanCheckTime = Ctx.CurrentTime;
  }
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

FVector UNavigationComponent::GetProjectedTargetForPlayer(
    const FVector& PlayerLoc) const {
  // 体积模式：找包围盒上离玩家最近的点 → 投影到 NavMesh
  if (bUseActorCenterAsDestination) {
    if (const FBox* Bounds = DestinationBounds.Find(ActiveTarget)) {
      const FVector ClosestOnBounds = Bounds->GetClosestPointTo(PlayerLoc);
      FNavLocation Proj;
      if (CachedNavSys && CachedNavSys->ProjectPointToNavigation(
                              ClosestOnBounds, Proj,
                              FVector(DestinationProjectionRadiusCm))) {
        return Proj.Location;
      }
    }
  }
  // 回退：直接投影中心
  FNavLocation Proj;
  if (CachedNavSys && CachedNavSys->ProjectPointToNavigation(
                          ActiveTargetLocation, Proj,
                          FVector(DestinationProjectionRadiusCm))) {
    return Proj.Location;
  }
  return ActiveTargetLocation;
}

void UNavigationComponent::TurnPlayer(float Degrees) {
  AActor *Owner = GetOwner();
  if (!Owner) return;
  Owner->AddActorLocalRotation(FRotator(0, Degrees, 0));
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
    {TEXT("TTSSpeedMultiplier"),             FRemoteConfigField::Float},
    {TEXT("RotateBeepBaseFreqHz"),           FRemoteConfigField::Float},
    {TEXT("RotateBeepFreqRangeHz"),          FRemoteConfigField::Float},
    {TEXT("RotatePanStrength"),              FRemoteConfigField::Float},
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
      else if (FCString::Strcmp(Field.Key, TEXT("PromptGapSeconds")) == 0) {
        if (SoundComp) SoundComp->PromptGapSeconds = FValue;
      }
      else if (FCString::Strcmp(Field.Key, TEXT("RouteDeviationThresholdMeters")) == 0)
        RouteDeviationThresholdMeters = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("RouteReplanCheckIntervalSeconds")) == 0)
        RouteReplanCheckIntervalSeconds = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("WaypointReachRadiusMeters")) == 0)
        WaypointReachRadiusMeters = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("TTSSpeedMultiplier")) == 0) {
        if (SoundComp) SoundComp->TTSSpeedMultiplier = FMath::Max(FValue, 0.1f);
      }
      else if (FCString::Strcmp(Field.Key, TEXT("RotateBeepBaseFreqHz")) == 0) {
        if (SoundComp) SoundComp->RotateBeepBaseFreqHz = FValue;
      }
      else if (FCString::Strcmp(Field.Key, TEXT("RotateBeepFreqRangeHz")) == 0) {
        if (SoundComp) SoundComp->RotateBeepFreqRangeHz = FValue;
      }
      else if (FCString::Strcmp(Field.Key, TEXT("RotatePanStrength")) == 0) {
        if (SoundComp) SoundComp->RotatePanStrength = FMath::Clamp(FValue, 0.0f, 2.0f);
      }
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
  StopDrip();
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

void UNavigationComponent::DrawForwardIndicator(AActor *Owner) {
  if (!bDrawForwardIndicator || !Owner || !GetWorld()) return;

  const FVector Start = Owner->GetActorLocation() + FVector(0, 0, 20.f);
  const FVector Forward = Owner->GetActorForwardVector();
  const FVector End = Start + Forward * ForwardIndicatorLengthCm;

  // 用 UE 自带的箭头绘制函数，自动渲染出带填充三角箭头的效果
  DrawDebugDirectionalArrow(GetWorld(), Start, End,
                            ForwardIndicatorArrowSizeCm * 4.0f,  // 箭头大小
                            FColor::Cyan, false, -1.f, 0, 4.0f   // 线粗 4
  );
}

void UNavigationComponent::HandleDebugKeyboardInput(AActor *Owner, float DeltaTime) {
  APawn *Pawn = Cast<APawn>(Owner);
  if (!Pawn) return;
  APlayerController *PC = GetWorld()->GetFirstPlayerController();
  if (!PC) return;
  // Q/E：和屏幕按钮一样的按住持续旋转
  if (PC->IsInputKeyDown(EKeys::Q)) TurnPlayer(-DebugTurnRateDegreesPerSec * DeltaTime);
  if (PC->IsInputKeyDown(EKeys::E)) TurnPlayer(DebugTurnRateDegreesPerSec * DeltaTime);
  // R：沿当前朝向行走
  if (PC->IsInputKeyDown(EKeys::R)) Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.0f);
}

void UNavigationComponent::TickComponent(
    float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction *ThisTickFunction) {
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  AActor *Owner = GetOwner();
  if (!Owner) return;

  if (bEnableDebugKeyboardControl) HandleDebugKeyboardInput(Owner, DeltaTime);

  // 屏幕按钮按住持续旋转（标记由 OnPressed/OnReleased 控制，每帧不清零）
  if (bIsTurningLeft) TurnPlayer(-DebugTurnRateDegreesPerSec * DeltaTime);
  if (bIsTurningRight) TurnPlayer(DebugTurnRateDegreesPerSec * DeltaTime);

  // 绘制人物朝向指示线+箭头
  DrawForwardIndicator(Owner);

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
    return;
  }

  // 填充上下文
  NavCtx.PlayerLoc = Owner->GetActorLocation();
  NavCtx.PlayerForward = Owner->GetActorForwardVector();
  NavCtx.PlayerRight = Owner->GetActorRightVector();
  NavCtx.CurrentTime = CurrentTime;

  // 动态更新最后路点：只剩终点时，每帧重新计算投影点
  const bool bOnFinalWaypoint =
      bUseActorCenterAsDestination && PlannedWaypoints.Num() > 0 &&
      CurrentWaypointIndex == PlannedWaypoints.Num() - 1;
  if (bOnFinalWaypoint) {
    PlannedWaypoints.Last() = GetProjectedTargetForPlayer(NavCtx.PlayerLoc);
  }

  // 到达：只剩终点时用投影点距离，否则用体积中心距离
  const FVector ArrivalRef =
      bOnFinalWaypoint ? PlannedWaypoints.Last() : ActiveTargetLocation;
  const float DirectDist =
      FVector::Dist2D(NavCtx.PlayerLoc, ArrivalRef) / 100.0f / DistanceScale;

  // 到达
  if (DirectDist <= ArrivalDistanceMeters) {
    // 清掉之前的提示，只播到达信息
    ClearNonCriticalPrompts();
    const FVector ToTarget2D =
        (ArrivalRef - NavCtx.PlayerLoc).GetSafeNormal2D();
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
    // 播放到达终点音效
    SendSoundEffect(ENavSoundCategory::SFX_Arrival);
    FinishNavigation(true);
    LastPlayerLocation = NavCtx.PlayerLoc;
    return;
  }

  StateMachine.Tick(*this, NavCtx);

  // 偏差检测（仅在正常导航中）
  if (CurrentWaypointIndex >= 0 && PlannedWaypoints.Num() >= 2 &&
      (CurrentTime - LastReplanCheckTime) > RouteReplanCheckIntervalSeconds) {
    LastReplanCheckTime = CurrentTime;

    // 检测 A：全局实时路径比计划路径距离短很多 → 偏移
    const FVector LivePathEnd = GetProjectedTargetForPlayer(NavCtx.PlayerLoc);
    UNavigationPath *LivePath = CachedNavSys->FindPathToLocationSynchronously(
        GetWorld(), NavCtx.PlayerLoc, LivePathEnd);
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
      // 播放偏离/错误音效
      SendSoundEffect(ENavSoundCategory::SFX_Deviation);
      CurrentWaypointIndex = -1;
      LastReplanCheckTime = CurrentTime;
    }
  }

  // 未就绪则跳过
  if (CurrentWaypointIndex < 0 || PlannedWaypoints.Num() < 2) {
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
      SendSoundEffect(ENavSoundCategory::SFX_WaypointReached);
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

  LastPlayerLocation = NavCtx.PlayerLoc;
}
