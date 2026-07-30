#include "TurnControlsWidget.h"

#include "NavigationComponent.h"

void UTurnControlsWidget::SetNavigationComponent(
    UNavigationComponent *InNavComp) {
  NavComp = InNavComp;
}

void UTurnControlsWidget::NativeConstruct() {
  Super::NativeConstruct();

  // 绑定蓝图中的按钮事件
  if (BtnLeft) {
    BtnLeft->OnPressed.AddDynamic(this, &UTurnControlsWidget::OnTurnLeftPressed);
    BtnLeft->OnReleased.AddDynamic(this, &UTurnControlsWidget::OnTurnLeftReleased);
  }
  if (BtnRight) {
    BtnRight->OnPressed.AddDynamic(this, &UTurnControlsWidget::OnTurnRightPressed);
    BtnRight->OnReleased.AddDynamic(this, &UTurnControlsWidget::OnTurnRightReleased);
  }
}

void UTurnControlsWidget::NativeDestruct() {
  if (BtnLeft) {
    BtnLeft->OnPressed.RemoveDynamic(this, &UTurnControlsWidget::OnTurnLeftPressed);
    BtnLeft->OnReleased.RemoveDynamic(this, &UTurnControlsWidget::OnTurnLeftReleased);
  }
  if (BtnRight) {
    BtnRight->OnPressed.RemoveDynamic(this, &UTurnControlsWidget::OnTurnRightPressed);
    BtnRight->OnReleased.RemoveDynamic(this, &UTurnControlsWidget::OnTurnRightReleased);
  }
  Super::NativeDestruct();
}

void UTurnControlsWidget::OnTurnLeftPressed() {
  if (NavComp) NavComp->StartTurnLeft();
}

void UTurnControlsWidget::OnTurnLeftReleased() {
  if (NavComp) NavComp->StopTurnLeft();
}

void UTurnControlsWidget::OnTurnRightPressed() {
  if (NavComp) NavComp->StartTurnRight();
}

void UTurnControlsWidget::OnTurnRightReleased() {
  if (NavComp) NavComp->StopTurnRight();
}
