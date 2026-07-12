#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "NavigationPath.h"
#include "NavigationStateMachine.h"

#include "NavigationComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNavigationStarted, FName,
                                            DestinationTag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNavigationArrived, FName,
                                             DestinationTag, bool, bSuccess);

// 远程导航参数配置应用结果
// - bSuccess：是否成功解析并应用
// - AppliedCount：本次成功覆盖的字段数量（缺字段/类型错误的不计）
// - Message：人类可读结果描述（供 UI 显示）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FOnRemoteConfigApplied, bool, bSuccess, int32, AppliedCount,
    const FString&, Message);

class UNavigationSystemV1;

UCLASS(ClassGroup = (Navigation), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UNavigationComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UNavigationComponent();

protected:
  virtual void BeginPlay() override;

public:
  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  bool NavigateTo(FName DestinationTag);

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  void StopNavigation();

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  TArray<FName> GetAvailableDestinations() const;

  UFUNCTION(BlueprintCallable, Category = "Navigation|Command")
  FName GetCurrentTarget() const { return ActiveTarget; }

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

  // 蜂鸣更新节流（秒）。Move 状态会根据 Progress 动态缩短。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float BeepUpdateIntervalSeconds = 1.0f;

  // TTS 句子之间的间隔（秒）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float PromptGapSeconds = 0.2f;

  // 路线规划：实时路径与固定路点的距离差超过此值 → 偏离，重新规划（米）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float RouteDeviationThresholdMeters = 2.0f;

  // 路线偏差检查间隔（秒）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float RouteReplanCheckIntervalSeconds = 1.5f;

  // 到达路点的判定半径（米）。
  UPROPERTY(EditAnywhere, Category = "Navigation|StateMachine")
  float WaypointReachRadiusMeters = 0.2f;

  UPROPERTY(EditAnywhere, Category = "Navigation|Command")
  bool bShowDebugMessages = true;

  // 电脑端调试：用 Q/E 转向，R 按当前朝向开始执行。
  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  bool bEnableDebugKeyboardControl = true;

  UPROPERTY(EditAnywhere, Category = "Navigation|Debug")
  float DebugTurnRateDegreesPerSec = 45.0f;

  // ── 状态机（friend 给各状态类访问内部） ─────────────────────────────
  friend class FPlanState;
  friend class FRotateState;
  friend class FMoveState;
  friend class FNavStateMachine;

private:
  void RefreshDestinationMap();
  void FinishNavigation(bool bSuccess);
  float EstimatePromptDurationSeconds(const FString &Message) const;
  float ComputePathDistanceMeters(const TArray<FVector> &PathPoints) const;

  // TTS 队列
  void EnqueuePrompt(const FString &Message, bool bInsertFirst = false);
  void ClearNonCriticalPrompts();
  void ProcessPromptScheduler(float CurrentTime);

  // 蜂鸣
  // Pan: -1=全左, 0=居中, +1=全右。Volume: 0..1 总音量缩放。
  void SendBeepCommand(bool bActive, int32 FreqHz, float Pan = 0.0f, float Volume = 1.0f);
  void StopBeep();
  void PlayLocalBeep(bool bActive, int32 FreqHz, float Pan, float Volume, float IntervalMs);

  // 调试
  void HandleDebugKeyboardInput(AActor *Owner, float DeltaTime);

  // 状态机运行时
  FNavStateMachine StateMachine;
  FNavContext NavCtx;
  int32 CurrentWaypointIndex = -1;  // 下一个要去的路点索引（-1 未规划，0 是起点）
  TArray<FVector> PlannedWaypoints;
  float LastReplanCheckTime = 0.0f;
  float LastBeepSendTime = 0.0f;

  // 导航目标
  TMap<FName, FVector> DestinationMap;
  UNavigationSystemV1 *CachedNavSys = nullptr;
  FName ActiveTarget = NAME_None;
  FVector ActiveTargetLocation = FVector::ZeroVector;
  bool bIsNavigating = false;

  // TTS 队列
  TArray<FString> PendingPrompts;
  FString CurrentRealtimePrompt;
  bool bHasRealtimePromptPending = false;
  float NextPromptDispatchTime = 0.0f;

  // 调试用
  FVector LastPlayerLocation = FVector::ZeroVector;
};
