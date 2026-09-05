#include "NavigationComponent.h"

#include "Blueprint/UserWidget.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "CameraModeWidget.h"
#include "DrawDebugHelpers.h"
#include "TurnControlsWidget.h"
#include "GameFramework/Character.h"
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
  InitializeCameraModes();

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
  if (CameraModeWidgetInstance) {
    CameraModeWidgetInstance->RemoveFromParent();
    CameraModeWidgetInstance = nullptr;
  }
  if (FreeCameraActor) {
    FreeCameraActor->Destroy();
    FreeCameraActor = nullptr;
  }
  if (FirstPersonViewActor) {
    FirstPersonViewActor->Destroy();
    FirstPersonViewActor = nullptr;
  }
  if (ThirdPersonViewActor) {
    ThirdPersonViewActor->Destroy();
    ThirdPersonViewActor = nullptr;
  }
  if (TurnControlsWidgetInstance) {
    TurnControlsWidgetInstance->RemoveFromParent();
    TurnControlsWidgetInstance = nullptr;
  }
  Super::EndPlay(EndPlayReason);
}

void UNavigationComponent::InitializeCameraModes() {
  if (bCameraModesInitialized) return;

  AActor *Owner = GetOwner();
  UWorld *World = GetWorld();
  APlayerController *PC = World ? World->GetFirstPlayerController() : nullptr;
  if (!Owner || !World || !PC) return;

  // 复用蓝图里现有的第三人称相机，确保保留当前俯视距离和角度。
  ThirdPersonCamera = Owner->FindComponentByClass<UCameraComponent>();
  if (!ThirdPersonCamera && Owner->GetRootComponent()) {
    ThirdPersonCamera = NewObject<UCameraComponent>(Owner, TEXT("ThirdPersonCamera"));
    ThirdPersonCamera->SetupAttachment(Owner->GetRootComponent());
    ThirdPersonCamera->SetRelativeLocation(FVector(0.0f, 0.0f, 600.0f));
    ThirdPersonCamera->SetRelativeRotation(FRotator(-60.0f, 0.0f, 0.0f));
    ThirdPersonCamera->RegisterComponent();
  }

  if (Owner->GetRootComponent()) {
    FirstPersonCamera = NewObject<UCameraComponent>(Owner, TEXT("FirstPersonCamera"));
    FirstPersonCamera->SetupAttachment(Owner->GetRootComponent());

    float HalfHeight = 90.0f;
    if (const ACharacter *Character = Cast<ACharacter>(Owner)) {
      if (const UCapsuleComponent *Capsule = Character->GetCapsuleComponent()) {
        HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
      }
    }

    // 角色原点通常位于胶囊体中心，将离地高度换算为相对胶囊中心的高度。
    const float FirstPersonHeight =
        HalfHeight * (FirstPersonHeightRatio * 2.0f - 1.0f);
    FirstPersonCamera->SetRelativeLocation(
        FVector(FirstPersonForwardOffsetCm, 0.0f, FirstPersonHeight));
    FirstPersonCamera->SetRelativeRotation(FRotator::ZeroRotator);
    FirstPersonCamera->bUsePawnControlRotation = false;
    FirstPersonCamera->bAutoActivate = false;
    if (ThirdPersonCamera) {
      FirstPersonCamera->SetFieldOfView(ThirdPersonCamera->FieldOfView);
    }
    FirstPersonCamera->RegisterComponent();

    FActorSpawnParameters FirstPersonSpawnParams;
    FirstPersonSpawnParams.Owner = Owner;
    FirstPersonSpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FirstPersonViewActor = World->SpawnActor<ACameraActor>(
        FVector::ZeroVector, FRotator::ZeroRotator, FirstPersonSpawnParams);
    if (FirstPersonViewActor) {
      FirstPersonViewActor->AttachToComponent(
          Owner->GetRootComponent(),
          FAttachmentTransformRules::KeepRelativeTransform);
      FirstPersonViewActor->SetActorRelativeLocation(
          FVector(FirstPersonForwardOffsetCm, 0.0f, FirstPersonHeight));
      FirstPersonViewActor->SetActorRelativeRotation(FRotator::ZeroRotator);
      if (FirstPersonViewActor->GetCameraComponent() && ThirdPersonCamera) {
        FirstPersonViewActor->GetCameraComponent()->SetFieldOfView(
            ThirdPersonCamera->FieldOfView);
      }
    }
  }

  if (ThirdPersonCamera) {
    const FVector InitialLocation = ThirdPersonCamera->GetComponentLocation();
    const FRotator InitialRotation = ThirdPersonCamera->GetComponentRotation();
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = Owner;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FreeCameraActor = World->SpawnActor<ACameraActor>(
        InitialLocation, InitialRotation, SpawnParams);
    if (FreeCameraActor && FreeCameraActor->GetCameraComponent()) {
      FreeCameraActor->GetCameraComponent()->SetFieldOfView(
          ThirdPersonCamera->FieldOfView);
    }

    FActorSpawnParameters ThirdPersonSpawnParams;
    ThirdPersonSpawnParams.Owner = Owner;
    ThirdPersonSpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ThirdPersonViewActor = World->SpawnActor<ACameraActor>(
        InitialLocation, InitialRotation, ThirdPersonSpawnParams);
    if (ThirdPersonViewActor) {
      ThirdPersonViewActor->AttachToComponent(
          ThirdPersonCamera,
          FAttachmentTransformRules::KeepWorldTransform);
      if (ThirdPersonViewActor->GetCameraComponent()) {
        ThirdPersonViewActor->GetCameraComponent()->SetFieldOfView(
            ThirdPersonCamera->FieldOfView);
      }
    }
  }

  // 让所有相机跟随视口宽高比（不锁定 16:9），避免非 16:9 视口出现上下黑边。
  // 注意：ACameraActor 的相机默认会锁定 AspectRatio，必须显式关闭。
  {
    UCameraComponent *Cameras[] = {
        ThirdPersonCamera,
        FirstPersonCamera,
        FreeCameraActor ? FreeCameraActor->GetCameraComponent() : nullptr,
        FirstPersonViewActor ? FirstPersonViewActor->GetCameraComponent()
                             : nullptr,
        ThirdPersonViewActor ? ThirdPersonViewActor->GetCameraComponent()
                             : nullptr,
    };
    for (UCameraComponent *Cam : Cameras) {
      if (Cam) Cam->bConstrainAspectRatio = false;
    }
  }

  if (CameraModeWidgetClass) {
    CameraModeWidgetInstance =
        CreateWidget<UCameraModeWidget>(PC, CameraModeWidgetClass);
  }
  if (CameraModeWidgetInstance) {
    CameraModeWidgetInstance->SetNavigationComponent(this);
    // 蓝图使用全屏 Canvas 的顶部居中锚点；空白区域不拦截游戏输入。
    CameraModeWidgetInstance->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
    CameraModeWidgetInstance->AddToViewport(20);

    FInputModeGameAndUI InputMode;
    InputMode.SetHideCursorDuringCapture(false);
    PC->SetInputMode(InputMode);
    PC->bShowMouseCursor = true;
  } else if (bShowDebugMessages) {
    ShowNavDebugMessage(
        10001,
        CameraModeWidgetClass ? TEXT("CameraUI: CreateWidget failed")
                              : TEXT("CameraUI: Assign CameraModeWidgetClass in NavigationComponent"),
        FColor::Red, 8.0f);
  }

  SetCameraMode(1);
  bCameraModesInitialized = true;
}

void UNavigationComponent::SetCameraMode(int32 Mode) {
  AActor *Owner = GetOwner();
  APlayerController *PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
  if (!Owner || !PC) return;

  const int32 ClampedMode = FMath::Clamp(Mode, 0, 2);
  ActiveCameraMode = static_cast<ECameraMode>(ClampedMode);
  bFreePointerDown = false;

  if (ActiveCameraMode == ECameraMode::Free && FreeCameraActor) {
    // 第一次切换到自由视角时，以玩家当前的第三人称相机位置为起点。
    if (!bFreeCameraInitialized && ThirdPersonCamera) {
      FreeCameraActor->SetActorLocation(ThirdPersonCamera->GetComponentLocation());
      FreeCameraActor->SetActorRotation(ThirdPersonCamera->GetComponentRotation());
      bFreeCameraInitialized = true;
    }
    if (FirstPersonCamera) FirstPersonCamera->SetActive(false);
    if (ThirdPersonCamera) ThirdPersonCamera->SetActive(false);
    PC->SetViewTargetWithBlend(FreeCameraActor, 0.15f,
                               VTBlend_EaseInOut);
    return;
  }

  if (ActiveCameraMode == ECameraMode::Free) {
    // 自由相机创建失败时保留可用的第三人称视角。
    ActiveCameraMode = ECameraMode::ThirdPerson;
  }

  if (FirstPersonCamera) FirstPersonCamera->SetActive(false);
  if (ThirdPersonCamera) ThirdPersonCamera->SetActive(false);

  AActor* ViewTarget = ActiveCameraMode == ECameraMode::FirstPerson
                           ? FirstPersonViewActor.Get()
                           : ThirdPersonViewActor.Get();
  if (!ViewTarget) {
    // 相机 Actor 创建失败时保留原来的玩家视图作为降级路径。
    ViewTarget = Owner;
  }
  PC->SetViewTargetWithBlend(ViewTarget, 0.15f, VTBlend_EaseInOut);
}

void UNavigationComponent::PanFreeCamera(const FVector2D &ScreenDelta) {
  if (ActiveCameraMode != ECameraMode::Free || !FreeCameraActor) return;

  const FRotator CameraYaw(0.0f, FreeCameraActor->GetActorRotation().Yaw, 0.0f);
  const FVector Right = CameraYaw.RotateVector(FVector::RightVector);
  const FVector Forward = CameraYaw.RotateVector(FVector::ForwardVector);
  const FVector WorldDelta =
      (-Right * ScreenDelta.X + Forward * ScreenDelta.Y) *
      FreeCameraPanSpeed;
  FreeCameraActor->AddActorWorldOffset(
      FVector(WorldDelta.X, WorldDelta.Y, 0.0f), false);
}

void UNavigationComponent::UpdateFreeCameraInput() {
  if (ActiveCameraMode != ECameraMode::Free || !GetWorld()) {
    bFreePointerDown = false;
    return;
  }

  APlayerController *PC = GetWorld()->GetFirstPlayerController();
  if (!PC) return;

  FVector2D PointerPosition = FVector2D::ZeroVector;
  bool bPointerPressed = false;
  if (PC->IsInputKeyDown(EKeys::LeftMouseButton)) {
    float MouseX = 0.0f;
    float MouseY = 0.0f;
    bPointerPressed = PC->GetMousePosition(MouseX, MouseY);
    PointerPosition = FVector2D(MouseX, MouseY);
  } else {
    float TouchX = 0.0f;
    float TouchY = 0.0f;
    PC->GetInputTouchState(ETouchIndex::Touch1, TouchX, TouchY,
                           bPointerPressed);
    PointerPosition = FVector2D(TouchX, TouchY);
  }

  if (!bPointerPressed) {
    bFreePointerDown = false;
    return;
  }

  if (bFreePointerDown) {
    PanFreeCamera(PointerPosition - LastFreePointerPosition);
  }
  LastFreePointerPosition = PointerPosition;
  bFreePointerDown = true;
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
    {TEXT("RouteDeviationThresholdA"),       FRemoteConfigField::Float},
    {TEXT("RouteReplanCheckIntervalSeconds"), FRemoteConfigField::Float},
    {TEXT("WaypointReachRadiusMeters"),      FRemoteConfigField::Float},
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
      else if (FCString::Strcmp(Field.Key, TEXT("RouteDeviationThresholdA")) == 0)
        RouteDeviationThresholdA = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("RouteReplanCheckIntervalSeconds")) == 0)
        RouteReplanCheckIntervalSeconds = FValue;
      else if (FCString::Strcmp(Field.Key, TEXT("WaypointReachRadiusMeters")) == 0)
        WaypointReachRadiusMeters = FValue;
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

float UNavigationComponent::GetEffectiveExecuteDriftDegrees(
    const FVector &PlayerLoc) {
  constexpr float FullProbeRadiusCm = 100.0f;
  // 阈值收紧区间：最终宽度 >= 0.5m 不限制，<= 0.2m 达最大限制（阈值一半）。
  constexpr float RestrictionStartCm = 50.0f;  // 0.5m 开始限制
  constexpr float RestrictionMaxCm = 20.0f;    // 0.2m 最大限制
  // 8 个方向（每 45° 一个），与对向成对判断是否身处窄通道。
  constexpr int32 DirectionSampleCount = 8;
  constexpr int32 BinarySearchSteps = 6;
  constexpr float ProjectionToleranceCm = 5.0f;
  constexpr float ProjectionHeightCm = 100.0f;

  if (!CachedNavSys || !GetWorld()) {
    return ExecuteDriftDegrees;
  }

  const float CurrentTime = GetWorld()->GetTimeSeconds();
  constexpr float ProbeRefreshIntervalSeconds = 1.0f;
  const bool bProbeCacheValid =
      CachedEffectiveDriftDegrees >= 0.0f &&
      FMath::IsNearlyEqual(LastDriftProbeBaseDegrees, ExecuteDriftDegrees) &&
      CurrentTime - LastDriftProbeTime < ProbeRefreshIntervalSeconds;
  if (bProbeCacheValid) {
    return CachedEffectiveDriftDegrees;
  }

  // 探测 8 个方向的可通行半径。
  float ClearanceCm[DirectionSampleCount];
  for (int32 i = 0; i < DirectionSampleCount; ++i) {
    const float Angle = 2.0f * PI * static_cast<float>(i) /
                        static_cast<float>(DirectionSampleCount);
    const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
    float LowCm = 0.0f;
    float HighCm = FullProbeRadiusCm;

    for (int32 Step = 0; Step < BinarySearchSteps; ++Step) {
      const float TestRadiusCm = (LowCm + HighCm) * 0.5f;
      const FVector TestPoint = PlayerLoc + Direction * TestRadiusCm;
      FNavLocation ProjectedLocation;
      const bool bReachable = CachedNavSys->ProjectPointToNavigation(
          TestPoint, ProjectedLocation,
          FVector(ProjectionToleranceCm, ProjectionToleranceCm,
                  ProjectionHeightCm));
      if (bReachable) {
        LowCm = TestRadiusCm;
      } else {
        HighCm = TestRadiusCm;
      }
    }
    ClearanceCm[i] = LowCm;
  }

  // 窄通道判定：某方向与其对向（180°）取较大值——只有同一轴线两端都近
  // （真正的通道）才会得到小值，单侧贴墙、另一侧开阔不会被限制；
  // 再对 8 个方向取最小值，得到最终可通行宽度。
  float NarrowestPairClearanceCm = FullProbeRadiusCm;
  for (int32 i = 0; i < DirectionSampleCount; ++i) {
    const int32 OppositeIndex =
        (i + DirectionSampleCount / 2) % DirectionSampleCount;
    const float PairWiderSideCm =
        FMath::Max(ClearanceCm[i], ClearanceCm[OppositeIndex]);
    NarrowestPairClearanceCm =
        FMath::Min(NarrowestPairClearanceCm, PairWiderSideCm);
  }

  // 最终宽度 >= 0.5m 使用完整配置值，<= 0.2m 使用配置值的一半，中间线性缩放。
  const float ClearanceAlpha = FMath::Clamp(
      (NarrowestPairClearanceCm - RestrictionMaxCm) /
          (RestrictionStartCm - RestrictionMaxCm),
      0.0f, 1.0f);
  const float ThresholdScale = FMath::Lerp(0.5f, 1.0f, ClearanceAlpha);
  CachedEffectiveDriftDegrees = ExecuteDriftDegrees * ThresholdScale;
  LastDriftProbeTime = CurrentTime;
  LastDriftProbeBaseDegrees = ExecuteDriftDegrees;
  return CachedEffectiveDriftDegrees;
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

  if (!bCameraModesInitialized) InitializeCameraModes();
  UpdateFreeCameraInput();

  // 绘制人物朝向指示线+箭头
  DrawForwardIndicator(Owner);

  const float CurrentTime = GetWorld()->GetTimeSeconds();
  const float EffectiveDriftDegrees =
      GetEffectiveExecuteDriftDegrees(Owner->GetActorLocation());

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

    GEngine->AddOnScreenDebugMessage(
        10000, 0.f, FColor::Yellow,
        FString::Printf(TEXT("Angle drift threshold: %.1f deg | base: %.1f deg"),
                        EffectiveDriftDegrees, ExecuteDriftDegrees));
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

    // 规则：当前待走路点被障碍物挡住时，强制重新规划。
    const int32 FirstWaypointIndex =
        FMath::Clamp(CurrentWaypointIndex, 1, PlannedWaypoints.Num() - 1);
    // ActorLocation 对 Character 通常是胶囊体中心，统一使用离地 20cm 的高度。
    constexpr float TraceHeightAboveGroundCm = 20.0f;
    float TraceHeight = NavCtx.PlayerLoc.Z;
    if (const ACharacter *Character = Cast<ACharacter>(Owner)) {
      if (const UCapsuleComponent *Capsule = Character->GetCapsuleComponent()) {
        TraceHeight -= Capsule->GetScaledCapsuleHalfHeight() -
                       TraceHeightAboveGroundCm;
      }
    }
    FVector TraceStart = NavCtx.PlayerLoc;
    TraceStart.Z = TraceHeight;
    FVector TraceEnd = PlannedWaypoints[FirstWaypointIndex];
    TraceEnd.Z += TraceHeightAboveGroundCm;

    FCollisionQueryParams TraceParams(
        FName(TEXT("NavigationFirstWaypointTrace")), true);
    TraceParams.AddIgnoredActor(Owner);
    const FCollisionObjectQueryParams ObjectQueryParams(
        FCollisionObjectQueryParams::AllObjects);
    FHitResult TraceHit;
    const bool bFirstWaypointBlocked = GetWorld()->LineTraceSingleByObjectType(
        TraceHit, TraceStart, TraceEnd,
        ObjectQueryParams, TraceParams);

    // 检测 A：计划剩余距离与实时 NavMesh 最短距离的绝对差值过大 → 偏移
    const FVector LivePathEnd = GetProjectedTargetForPlayer(NavCtx.PlayerLoc);
    UNavigationPath *LivePath = CachedNavSys->FindPathToLocationSynchronously(
        GetWorld(), NavCtx.PlayerLoc, LivePathEnd);
    bool bDeviated = bFirstWaypointBlocked;
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

      bDeviated = bDeviated ||
                  FMath::Abs(PlannedRemain - LiveTotal) >
                      RouteDeviationThresholdA;
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
    const FVector NW =
        PlannedWaypoints[FMath::Min(CurrentWaypointIndex,
                                    PlannedWaypoints.Num() - 1)];
    DrawDebugLine(GetWorld(), NavCtx.PlayerLoc, NW, FColor::Yellow, false,
                  0.1f, 0, 1.0f);
  }

  LastPlayerLocation = NavCtx.PlayerLoc;
}
