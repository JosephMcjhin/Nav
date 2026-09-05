#include "CameraModeWidget.h"

#include "NavigationComponent.h"

void UCameraModeWidget::SetNavigationComponent(UNavigationComponent *InNavComp) {
  NavComp = InNavComp;
}

void UCameraModeWidget::NativeConstruct() {
  Super::NativeConstruct();

  // 绑定蓝图中的按钮事件
  if (BtnFirstPerson) {
    BtnFirstPerson->OnClicked.AddDynamic(
        this, &UCameraModeWidget::OnFirstPersonClicked);
  }
  if (BtnThirdPerson) {
    BtnThirdPerson->OnClicked.AddDynamic(
        this, &UCameraModeWidget::OnThirdPersonClicked);
  }
  if (BtnFreeView) {
    BtnFreeView->OnClicked.AddDynamic(
        this, &UCameraModeWidget::OnFreeViewClicked);
  }
}

void UCameraModeWidget::NativeDestruct() {
  if (BtnFirstPerson) {
    BtnFirstPerson->OnClicked.RemoveDynamic(
        this, &UCameraModeWidget::OnFirstPersonClicked);
  }
  if (BtnThirdPerson) {
    BtnThirdPerson->OnClicked.RemoveDynamic(
        this, &UCameraModeWidget::OnThirdPersonClicked);
  }
  if (BtnFreeView) {
    BtnFreeView->OnClicked.RemoveDynamic(
        this, &UCameraModeWidget::OnFreeViewClicked);
  }
  Super::NativeDestruct();
}

void UCameraModeWidget::OnFirstPersonClicked() {
  if (NavComp) NavComp->SetCameraMode(0);
}

void UCameraModeWidget::OnThirdPersonClicked() {
  if (NavComp) NavComp->SetCameraMode(1);
}

void UCameraModeWidget::OnFreeViewClicked() {
  if (NavComp) NavComp->SetCameraMode(2);
}
