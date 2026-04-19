// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "ServerConnectionComponent.h"

#include "BeaconCalibrationWidget.generated.h"

/**
 * UBeaconCalibrationWidget
 *
 *   1. Three-point UWB calibration (capture UE position + UWB coordinates)
 *   2. Triggering the server-side affine-transform solve
 *   3. A virtual joystick for manually moving the character to each sample
 * point
 *
 * Create a child Blueprint (e.g. WBP_BeaconCalibration), bind the named
 * widgets below, and call SetVoiceComponent() on BeginPlay.
 */
UCLASS(Blueprintable)
class PROJECT001_API UBeaconCalibrationWidget : public UUserWidget {
  GENERATED_BODY()

public:
  // ─── Bound UMG Widgets (name them the same in the Blueprint) ───────────
  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnCapture1;

  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnCapture2;

  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnCapture3;

  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnSolve;

  // Toolbar toggling
  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnToggleToolbar;

  UPROPERTY(meta = (BindWidgetOptional))
  class UWidget *ToolbarContainer;

  // Server Connection
  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnConnect;

  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnCalibrateHeading;

  UPROPERTY(meta = (BindWidgetOptional))
  UButton *BtnClearCache;

  // Status labels
  UPROPERTY(meta = (BindWidgetOptional))
  UTextBlock *TxtStatus;

  UPROPERTY(meta = (BindWidgetOptional))
  UTextBlock *TxtPoint1;

  UPROPERTY(meta = (BindWidgetOptional))
  UTextBlock *TxtPoint2;

  UPROPERTY(meta = (BindWidgetOptional))
  UTextBlock *TxtPoint3;

  // Server IP input
  UPROPERTY(meta = (BindWidgetOptional))
  class UEditableTextBox *TxtServerIP;

  // ─── Public API ────────────────────────────────────────────────────────

  /** Call this from the owning Character's BeginPlay (or via Blueprint). */
  UFUNCTION(BlueprintCallable, Category = "Beacon Calibration")
  void SetConnectionComponent(UServerConnectionComponent *InComponent);

  /** Capture the current UE position as sample point N (1-3). */
  UFUNCTION(BlueprintCallable, Category = "Beacon Calibration")
  void CapturePoint(int32 PointIndex);

  /** Send /api/calibrate/solve to the server. */
  UFUNCTION(BlueprintCallable, Category = "Beacon Calibration")
  void SolveCalibration();

  /**
   * Move the owned character in 2D.
   *  X = right/left  (-1..1)
   *  Y = forward/back (-1..1)
   * Bind this to a virtual joystick axis in Blueprint.
   */
  UFUNCTION(BlueprintCallable, Category = "Beacon Calibration")
  void JoystickInput(float AxisX, float AxisY);

protected:
  virtual void NativeConstruct() override;
  virtual void NativeTick(const FGeometry &MyGeometry,
                          float InDeltaTime) override;

private:
  UPROPERTY()
  UServerConnectionComponent *ConnComp = nullptr;

  // Tracks which points have been captured (0 = not yet)
  int32 CapturedFlags = 0; // bitmask: bit0=P1, bit1=P2, bit2=P3

  // Accumulated joystick axes, applied each tick
  float JoystickX = 0.f;
  float JoystickY = 0.f;

  void RefreshUI();
  void SetStatus(const FString &Msg);
  void SwitchUIState(int32 State);

  // HTTP helper – fires-and-forgets a POST to the server
  void HttpPost(const FString &RelPath, const FString &JsonBody,
                TFunction<void(const FString &)> OnResponse = nullptr);

  // Internal button click handlers – bound via AddDynamic in NativeConstruct
  UFUNCTION()
  void OnCapture1();
  UFUNCTION()
  void OnCapture2();
  UFUNCTION()
  void OnCapture3();

  UFUNCTION()
  void OnSolve();

  UFUNCTION()
  void OnToggleToolbar();

  UFUNCTION()
  void OnWSConnected(const FString& URL);

  UFUNCTION()
  void OnWSConnectionError(const FString& Error);

  UFUNCTION()
  void OnServerStatusReceived(bool bIsCalibrated, bool bIsHeadingCalibrated, float ImuOffset, int32 Points);

  UFUNCTION()
  void OnConnectClicked();

  UFUNCTION()
  void OnCalibrateHeadingClicked();

  UFUNCTION()
  void OnClearCacheClicked();
};
