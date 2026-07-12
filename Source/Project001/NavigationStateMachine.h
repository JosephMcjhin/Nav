#pragma once

#include "CoreMinimal.h"

class UNavigationComponent;

// 状态机共享上下文：每帧由 NavigationComponent 填充，传给当前状态。
struct FNavContext {
  // 实时输入（每帧由 Component 填充）
  FVector PlayerLoc = FVector::ZeroVector;
  FVector PlayerForward = FVector::ForwardVector;
  FVector PlayerRight = FVector::RightVector;
  float CurrentTime = 0.0f;

  // 段信息（状态机 Tick 内部计算）
  FVector SegmentDir = FVector::ForwardVector;  // 玩家→下一路点
  float RemainingMeters = 0.0f;                 // 玩家→下一路点距离
  float AngleError = 0.0f;                      // +右/-左

  // 状态起始快照（OnEnter 时设置，状态类自己读）
  FVector StartLocation = FVector::ZeroVector;
};

// 状态标识
enum class ENavState : uint8 {
  None = 0,
  Plan = 1,
  Rotate = 2,
  Move = 3,
};

// 状态基类
class FNavState {
 public:
  virtual ~FNavState() = default;
  virtual void OnEnter(UNavigationComponent& Nav, FNavContext& Ctx) {}
  // 返回下一帧应处于的状态。返回自己表示保持。
  virtual void Tick(UNavigationComponent& Nav, FNavContext& Ctx) {}
  virtual void OnExit(UNavigationComponent& Nav, FNavContext& Ctx) {}
  virtual FName GetName() const = 0;
};

// 状态机容器
class FNavStateMachine {
 public:
  FNavStateMachine();

  void Tick(UNavigationComponent& Nav, FNavContext& Ctx);
  void Reset();

  ENavState GetCurrentState() const { return CurrentState; }
  FNavState* GetCurrentStatePtr() const {
    return States[static_cast<uint8>(CurrentState)].Get();
  }

 private:
  ENavState EvaluateState(UNavigationComponent& Nav, FNavContext& Ctx);
  void SwitchTo(ENavState NewState, UNavigationComponent& Nav, FNavContext& Ctx);

  ENavState CurrentState = ENavState::None;
  int32 LastWaypointIndex = -2;
  TUniquePtr<FNavState> States[4];
};
