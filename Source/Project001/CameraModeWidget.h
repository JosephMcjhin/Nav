#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "CoreMinimal.h"

#include "CameraModeWidget.generated.h"

class UNavigationComponent;

/**
 * 相机模式切换按钮控件（第一人称 / 第三人称 / 自由视角）。
 * 创建 Blueprint 子类（如 WBP_CameraMode），在蓝图中摆放三个按钮，
 * 变量名绑定 BtnFirstPerson / BtnThirdPerson / BtnFreeView，
 * 然后调用 SetNavigationComponent() 传入导航组件。
 */
UCLASS(Blueprintable)
class PROJECT001_API UCameraModeWidget : public UUserWidget {
  GENERATED_BODY()

public:
  /** 设置导航组件引用（必须在 AddToViewport 之前调用）。 */
  UFUNCTION(BlueprintCallable, Category = "Camera")
  void SetNavigationComponent(UNavigationComponent *InNavComp);

protected:
  virtual void NativeConstruct() override;
  virtual void NativeDestruct() override;

  // ─── 在 Blueprint 子类中绑定的按钮 ───────────────────────────────
  UPROPERTY(meta = (BindWidgetOptional))
  TObjectPtr<UButton> BtnFirstPerson;

  UPROPERTY(meta = (BindWidgetOptional))
  TObjectPtr<UButton> BtnThirdPerson;

  UPROPERTY(meta = (BindWidgetOptional))
  TObjectPtr<UButton> BtnFreeView;

private:
  UFUNCTION()
  void OnFirstPersonClicked();

  UFUNCTION()
  void OnThirdPersonClicked();

  UFUNCTION()
  void OnFreeViewClicked();

  TObjectPtr<UNavigationComponent> NavComp;
};
