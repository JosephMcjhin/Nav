#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UWBTargetComponent.generated.h"

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class PROJECT001_API UUWBTargetComponent : public UActorComponent {
  GENERATED_BODY()

public:
  UUWBTargetComponent();

  virtual void BeginPlay() override;

  virtual void
  TickComponent(float DeltaTime, ELevelTick TickType,
                FActorComponentTickFunction *ThisTickFunction) override;

  // UWB data drives ONLY the actor position.
  UFUNCTION(BlueprintCallable, Category = "UWB")
  void SetUWBTarget(float InX, float InY);

  // IMU data drives ONLY the actor rotation (yaw).
  UFUNCTION(BlueprintCallable, Category = "IMU")
  void SetIMURotation(float Yaw);

  UFUNCTION(BlueprintCallable, Category = "IMU")
  void ClearIMURotation();

private:
  FVector TargetLocation = FVector::ZeroVector;
  FVector SmoothedTargetLocation = FVector::ZeroVector;
  bool bHasTarget = false;
  double LastTargetUpdateTime = 0.0;
  FRotator TargetRotation = FRotator::ZeroRotator;
  FRotator SmoothedTargetRotation = FRotator::ZeroRotator;
  bool bHasRotation = false;
  float RotationInterpSpeedDegPerSec = 360.0f;
  float TargetSmoothingSpeed = 4.5f;
  float RotationTargetSmoothingSpeed = 6.0f;
  float AutoMoveDeadZoneCm = 4.0f;
  float TargetStreamHoldSeconds = 0.35f;
  float DefaultMaxWalkSpeed = 280.0f;
  double LastSpeedSampleTime = 0.0;

  // True on frames where manual input (gamepad joystick OR keyboard WASD)
  // is driving movement. While true we temporarily enable
  // bOrientRotationToMovement so the character faces its walk direction,
  // and we skip the IMU rotation block (manual input owns yaw then).
  bool bUsingManualInput = false;

  // Cached weak pointer to the movement component so we can flip
  // bOrientRotationToMovement on/off per-frame without re-casting.
  UPROPERTY()
  UCharacterMovementComponent *CachedMoveComp = nullptr;
};
