#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "NavigationPath.h"
#include "NavigationSoundPlayer.h"
#include "NavigationStateMachine.h"

#include "NavigationComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNavigationStarted, FName,
                                            DestinationTag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNavigationArrived, FName,
                                             DestinationTag, bool, bSuccess);

// 远程导航参数配置应用结果
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FOnRemoteConfigApplied, bool, bSuccess, int32, AppliedCount,
    const FString&, Message);

class UNavigationSystemV1;
class UNavigationQueryFilter;

UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UNavigationComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UNavigationComponent();

protected:
  virtual void BeginPlay() override;
  virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  bool NavigateTo(FName DestinationTag);

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  void StopNavigation();

  /** 手机端按钮调用：让角色原地转向指定角度（正=右转，负=左转）。 */
  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  void TurnPlayer(float Degrees);

  /** 开始持续左/右转（按钮按住时调用）。 */
  void StartTurnLeft() { bIsTurningLeft = true; }
  void StopTurnLeft()  { bIsTurningLeft = false; }
  void StartTurnRight(){ bIsTurningRight = true; }
  void StopTurnRight() { bIsTurningRight = false; }

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  TArray<FName> GetAvailableDestinations() const;

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  FName GetCurrentTarget() const { return ActiveTarget; }

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  bool IsNavigating() const { return bIsNavigating; }

  /** 偏离后全局重新寻路，由 PlanState::OnEnter / Tick 调用。 */
  void ReplanFromDeviation(const FNavContext& Ctx);

  // ── 音效接口（委托给 UNavigationSoundComponent） ──────────────────
  void SendSoundEffect(ENavSoundCategory Category) {
    if (SoundComp) SoundComp->SendSoundEffect(Category);
  }
  void SendBeepCommand(bool bActive, int32 FreqHz, float Pan = 0.0f,
                       float Volume = 1.0f,
                       ENavSoundCategory BeepType = ENavSoundCategory::Beep_TurnCalibrate) {
    if (SoundComp) SoundComp->SendBeepCommand(bActive, FreqHz, Pan, Volume, BeepType);
  }
  void StopBeep() { if (SoundComp) SoundComp->StopBeep(); }
  void SendDripCommand(bool bActive, float IntervalMs) {
    if (SoundComp) SoundComp->SendDripCommand(bActive, IntervalMs);
  }
  void StopDrip() { if (SoundComp) SoundComp->StopDrip(); }
  void EnqueuePrompt(const FString& Message, bool bHighPriority = false) {
    if (SoundComp) SoundComp->EnqueuePrompt(Message, bHighPriority);
  }
  void ClearNonCriticalPrompts() { if (SoundComp) SoundComp->ClearNonCriticalPrompts(); }

  /**
   * 从后端 nav_config.json 拉取的 JSON 字符串中应用导航参数配置。
   *
   * @param JsonString 后端返回的 JSON，期望格式：
   *        {"type":"nav_config","status":"ok","config":{
   *            "DistanceScale": 1.0,
   *            "ArrivalDistanceMeters": 0.2,
   *            ...
   *        }}}
   *        或直接 {"DistanceScale": 1.0, ...}
   *        未提供/类型错误的字段会被静默忽略并保留当前值。
   * @return 是否成功解析（至少应用 1 个字段才返回 true）。
   */
  UFUNCTION(BlueprintCallable, Category = "Navigation|RemoteConfig")
  bool ApplyRemoteConfig(const FString &JsonString);

  UPROPERTY(BlueprintAssignable, Category = "Navigation|Command")
  FOnNavigationStarted OnNavigationStarted;

  UPROPERTY(BlueprintAssignable, Category = "Navigation|Command")
  FOnNavigationArrived OnNavigationArrived;

  // 远程导航参数配置应用结果（成功/失败都会触发）
  UPROPERTY(BlueprintAssignable, Category = "Navigation|RemoteConfig")
  FOnRemoteConfigApplied OnRemoteConfigApplied;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  TArray<FName> DestinationTags;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float DistanceScale = 1.0f;

  UPROPERTY(EditAnywhere, Category = "Navigation")
  float ArrivalDistanceMeters = 0.2f;

  // ── 状态机参数 ───────────────────────────────────────────────────────────
  // 朝向校准：用户起始转身多少度后开始蜂鸣引导（度）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float AlignBeepStartDeltaDegrees = 10.0f;

  // 朝向校准通过的角度阈值（|angle error| < 此值视为已对准，进入 EXECUTE）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float AlignToleranceDegrees = 10.0f;

  // EXECUTE：用户走出多远（米）后开始蜂鸣引导。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float ExecuteBeepStartMeters = 0.4f;

  // EXECUTE：朝向偏移多少度时退回 ALIGN 状态。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float ExecuteDriftDegrees = 15.0f;

  // 朝向长时间不变（度）超过该阈值视为“卡住”，重新 TTS 提示一次（秒）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float AlignIdleRepromptSeconds = 2.5f;

  // 路线规划：检测A阈值。计划剩余距离与实时 NavMesh 最短距离的绝对差值 > 该值 → 偏离，重新规划（米）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float RouteDeviationThresholdA = 0.5f;

  // 路线偏差检查间隔（秒）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float RouteReplanCheckIntervalSeconds = 1.5f;

  // 到达路点的判定半径（米）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float WaypointReachRadiusMeters = 0.2f;

  // ── 目的地模式：true=直接用 Actor 中心；false=投影到碰撞体边缘 navmesh ──
  UPROPERTY(EditAnywhere, Category = "Navigation|Destination")
  bool bUseActorCenterAsDestination = true;

  /** 目的地投影到 NavMesh 的搜索半径（厘米）。
   *  NavModifierVolume 等会挖空内部 NavMesh，中心点不在 NavMesh 上，
   *  需要投影到最近的 NavMesh 边缘。默认 1000cm 足够覆盖 20m 见方的 Volume。 */
  UPROPERTY(EditAnywhere, Category = "Navigation|Destination")
  float DestinationProjectionRadiusCm = 1000.0f;

  /** 寻路过滤器（可选）。设置为道路偏好过滤器可让路线偏向道路中央。
   *  典型做法：在编辑器中创建 NavigationQueryFilter 资产，配置 AreaCost
   *  让边缘区域（窄通道）的 Cost 更高，使寻路自动偏向宽通道中央。
   *  留空则使用默认查询过滤器。 */
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Navigation|Pathfinding")
  TSubclassOf<UNavigationQueryFilter> PathfindingFilterClass;

  // ── 人物朝向指示器 ──────────────────────────────────────────────────────
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  bool bDrawForwardIndicator = true;

  // 朝向指示线长度（厘米）
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  float ForwardIndicatorLengthCm = 60.0f;

  // 箭头大小（厘米）
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  float ForwardIndicatorArrowSizeCm = 30.0f;

  UPROPERTY(EditAnywhere, Category = "Navigation|Command")
  bool bShowDebugMessages = true;

  // 电脑端调试：用 Q/E 转向，R 按当前朝向开始执行。
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  bool bEnableDebugKeyboardControl = true;

  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  float DebugTurnRateDegreesPerSec = 45.0f;

  // 手机端按钮单次转向角度（度）
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  float TurnStepDegrees = 15.0f;

  // 是否在屏幕上显示左/右转按钮（手机端用）
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  bool bShowTurnControls = true;

  /** 转向按钮的 Blueprint Widget 类（如 WBP_TurnControls）。
   *  在该 Blueprint 中摆放 ← → 两个 UButton，变量名绑定为 BtnLeft / BtnRight。 */
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  TSubclassOf<class UTurnControlsWidget> TurnControlsWidgetClass;

  // ── 状态机（friend 给各状态类访问内部） ─────────────────────────────
  friend class FPlanState;
  friend class FRotateState;
  friend class FMoveState;
  friend class FNavStateMachine;

private:
  void RefreshDestinationMap();
  void FinishNavigation(bool bSuccess);
  float ComputePathDistanceMeters(const TArray<FVector> &PathPoints) const;

  // 朝向指示器
  void DrawForwardIndicator(AActor *Owner);

  // 调试
  void HandleDebugKeyboardInput(AActor *Owner, float DeltaTime);

  // 持续转向标记（按钮/Q/E 按住时 true，松开时 false）
  bool bIsTurningLeft = false;
  bool bIsTurningRight = false;

  // 状态机运行时
  FNavStateMachine StateMachine;
  FNavContext NavCtx;
  int32 CurrentWaypointIndex = -1;  // 下一个要去的路点索引（-1 未规划，0 是起点）
  TArray<FVector> PlannedWaypoints;
  float LastReplanCheckTime = 0.0f;

  // 导航目标
  TMap<FName, FVector> DestinationMap;
  TMap<FName, FBox> DestinationBounds;  // 体积模式下的包围盒
  UNavigationSystemV1 *CachedNavSys = nullptr;
  FName ActiveTarget = NAME_None;
  FVector ActiveTargetLocation = FVector::ZeroVector;
  bool bIsNavigating = false;

  // 音效组件（需手动挂载到同一 Actor 上，BeginPlay 时自动查找）
  class UNavigationSoundComponent* SoundComp = nullptr;

  // 调试用
  FVector LastPlayerLocation = FVector::ZeroVector;

  // 转向控制按钮 Widget 实例
  TObjectPtr<UUserWidget> TurnControlsWidgetInstance;

  // 体积模式：找包围盒上离玩家最近的点 → 投影到 NavMesh
  FVector GetProjectedTargetForPlayer(const FVector& PlayerLoc) const;
};
