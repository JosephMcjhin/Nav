#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "CoreMinimal.h"

#include "TurnControlsWidget.generated.h"

class UNavigationComponent;

/**
 * 转向控制按钮控件。
 * 创建 Blueprint 子类（如 WBP_TurnControls），在蓝图中摆放 ← → 两个按钮，
 * 并将按钮变量绑定到对应的 UMG Button 控件。
 * 然后调用 SetNavigationComponent() 传入导航组件。
 */
UCLASS(Blueprintable)
class PROJECT001_API UTurnControlsWidget : public UUserWidget {
  GENERATED_BODY()

public:
  /** 设置导航组件引用（必须在 AddToViewport 之前调用）。 */
  UFUNCTION(BlueprintCallable, Category = "TurnControls")
  void SetNavigationComponent(UNavigationComponent *InNavComp);

protected:
  virtual void NativeConstruct() override;
  virtual void NativeDestruct() override;

  // ─── 在 Blueprint 子类中绑定的按钮 ───────────────────────────────
  UPROPERTY(meta = (BindWidgetOptional))
  TObjectPtr<UButton> BtnLeft;

  UPROPERTY(meta = (BindWidgetOptional))
  TObjectPtr<UButton> BtnRight;

private:
  UFUNCTION()
  void OnTurnLeftPressed();

  UFUNCTION()
  void OnTurnLeftReleased();

  UFUNCTION()
  void OnTurnRightPressed();

  UFUNCTION()
  void OnTurnRightReleased();

  void AutoSizeText(UButton* Button);

  TObjectPtr<UNavigationComponent> NavComp;
};
