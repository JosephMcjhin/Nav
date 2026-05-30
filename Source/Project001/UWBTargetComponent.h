#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
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

  UFUNCTION(BlueprintCallable, Category = "UWB")
  void SetUWBTarget(float InX, float InY);

  UFUNCTION(BlueprintCallable, Category = "UWB")
  void SetUWBRotation(float Yaw);

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
};
