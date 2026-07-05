#pragma once

#include "CoreMinimal.h"
#include "../NavigationStateMachine.h"

// Wait 状态：TTS 播放中时暂停，不触发 OnExit/OnEnter。
class FWaitState : public FNavState {
 public:
  ENavState Tick(UNavigationComponent& Nav, FNavContext& Ctx) override {
    return ENavState::Wait;
  }
  FName GetName() const override { return FName(TEXT("WAIT")); }
};
